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
    const kernel_abi_root_mod = b.createModule(.{
        .root_source_file = b.path("../userland/support/kernel_abi_root.zig"),
    });
    const persistent_fs_layout_mod = b.createModule(.{
        .root_source_file = b.path("../userland/support/persistent_fs_layout.zig"),
    });
    test_mod.addImport("kernel_abi_root", kernel_abi_root_mod);

    const efi_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = efi_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
    });
    efi_mod.addImport("kernel_abi_root", kernel_abi_root_mod);
    const efi_app = b.addExecutable(.{
        .name = "BOOTX64",
        .root_module = efi_mod,
    });

    const user_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .freestanding,
        .abi = .none,
    });
    const support_root_mod = b.createModule(.{
        .root_source_file = b.path("../userland/support/support_root.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    const init_app_mod = b.createModule(.{
        .root_source_file = b.path("../bootstrap/programs/init_app.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    init_app_mod.addImport("support_root", support_root_mod);
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

    const shell_mod = b.createModule(.{
        .root_source_file = b.path("../bootstrap/programs/shell.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    shell_mod.addImport("support_root", support_root_mod);
    shell_mod.strip = true;
    const shell_app = b.addExecutable(.{
        .name = "SHELL",
        .root_module = shell_mod,
    });
    shell_app.pie = true;
    shell_app.entry = .{ .symbol_name = "_start" };
    shell_app.link_z_common_page_size = 0x10;
    shell_app.link_z_max_page_size = 0x10;
    const install_shell = b.addInstallArtifact(shell_app, .{
        .dest_sub_path = "EFI/BOOT/SHELL.ELF",
    });
    const shell_step = b.step("shell-elf", "Build shell PIE ELF");
    shell_step.dependOn(&install_shell.step);

    const mouse_driver_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/mouse_driver.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    mouse_driver_mod.addImport("support_root", support_root_mod);
    mouse_driver_mod.strip = true;
    const mouse_driver_app = b.addExecutable(.{
        .name = "MOUSEDRV",
        .root_module = mouse_driver_mod,
    });
    mouse_driver_app.pie = true;
    mouse_driver_app.entry = .{ .symbol_name = "_start" };
    mouse_driver_app.link_z_common_page_size = 0x10;
    mouse_driver_app.link_z_max_page_size = 0x10;
    const install_mouse_driver = b.addInstallArtifact(mouse_driver_app, .{
        .dest_sub_path = "EFI/BOOT/MOUSEDRV.ELF",
    });
    const mouse_driver_step = b.step("mouse-driver-elf", "Build mouse driver PIE ELF");
    mouse_driver_step.dependOn(&install_mouse_driver.step);

    const keyboard_driver_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/keyboard_driver.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    keyboard_driver_mod.addImport("support_root", support_root_mod);
    keyboard_driver_mod.strip = true;
    const keyboard_driver_app = b.addExecutable(.{
        .name = "KEYBDRV",
        .root_module = keyboard_driver_mod,
    });
    keyboard_driver_app.pie = true;
    keyboard_driver_app.entry = .{ .symbol_name = "_start" };
    keyboard_driver_app.link_z_common_page_size = 0x10;
    keyboard_driver_app.link_z_max_page_size = 0x10;
    const install_keyboard_driver = b.addInstallArtifact(keyboard_driver_app, .{
        .dest_sub_path = "EFI/BOOT/KEYBDRV.ELF",
    });
    const keyboard_driver_step = b.step("keyboard-driver-elf", "Build keyboard driver PIE ELF");
    keyboard_driver_step.dependOn(&install_keyboard_driver.step);

    const bootlog_sender_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/bootlog_sender.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    bootlog_sender_mod.addImport("support_root", support_root_mod);
    bootlog_sender_mod.strip = true;
    const bootlog_sender_app = b.addExecutable(.{
        .name = "BLOGSND",
        .root_module = bootlog_sender_mod,
    });
    bootlog_sender_app.pie = true;
    bootlog_sender_app.entry = .{ .symbol_name = "_start" };
    bootlog_sender_app.link_z_common_page_size = 0x10;
    bootlog_sender_app.link_z_max_page_size = 0x10;
    const install_bootlog_sender = b.addInstallArtifact(bootlog_sender_app, .{
        .dest_sub_path = "EFI/BOOT/BLOGSND.ELF",
    });
    const bootlog_sender_step = b.step("bootlog-sender-elf", "Build bootlog sender PIE ELF");
    bootlog_sender_step.dependOn(&install_bootlog_sender.step);

    const mouse_button_demo_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/mouse_button_demo.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    mouse_button_demo_mod.addImport("support_root", support_root_mod);
    mouse_button_demo_mod.strip = true;
    const mouse_button_demo_app = b.addExecutable(.{
        .name = "MBTNDEMO",
        .root_module = mouse_button_demo_mod,
    });
    mouse_button_demo_app.pie = true;
    mouse_button_demo_app.entry = .{ .symbol_name = "_start" };
    mouse_button_demo_app.link_z_common_page_size = 0x10;
    mouse_button_demo_app.link_z_max_page_size = 0x10;
    const install_mouse_button_demo = b.addInstallArtifact(mouse_button_demo_app, .{
        .dest_sub_path = "EFI/BOOT/MBTNDEMO.ELF",
    });
    const mouse_button_demo_step = b.step("mouse-button-demo-elf", "Build mouse button demo PIE ELF");
    mouse_button_demo_step.dependOn(&install_mouse_button_demo.step);

    const terminal_window_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/terminal_window.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    terminal_window_mod.addImport("support_root", support_root_mod);
    terminal_window_mod.strip = true;
    const terminal_window_app = b.addExecutable(.{
        .name = "TERMWIN",
        .root_module = terminal_window_mod,
    });
    terminal_window_app.pie = true;
    terminal_window_app.entry = .{ .symbol_name = "_start" };
    terminal_window_app.link_z_common_page_size = 0x10;
    terminal_window_app.link_z_max_page_size = 0x10;
    const install_terminal_window = b.addInstallArtifact(terminal_window_app, .{
        .dest_sub_path = "EFI/BOOT/TERMWIN.ELF",
    });
    const terminal_window_step = b.step("terminal-window-elf", "Build terminal window PIE ELF");
    terminal_window_step.dependOn(&install_terminal_window.step);

    const taskbar_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/taskbar.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    taskbar_mod.addImport("support_root", support_root_mod);
    taskbar_mod.strip = true;
    const taskbar_app = b.addExecutable(.{
        .name = "TASKBAR",
        .root_module = taskbar_mod,
    });
    taskbar_app.pie = true;
    taskbar_app.entry = .{ .symbol_name = "_start" };
    taskbar_app.link_z_common_page_size = 0x10;
    taskbar_app.link_z_max_page_size = 0x10;
    const install_taskbar = b.addInstallArtifact(taskbar_app, .{
        .dest_sub_path = "EFI/BOOT/TASKBAR.ELF",
    });
    const taskbar_step = b.step("taskbar-elf", "Build taskbar PIE ELF");
    taskbar_step.dependOn(&install_taskbar.step);

    const launcher_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/launcher.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    launcher_mod.addImport("support_root", support_root_mod);
    launcher_mod.strip = true;
    const launcher_app = b.addExecutable(.{
        .name = "LAUNCHER",
        .root_module = launcher_mod,
    });
    launcher_app.pie = true;
    launcher_app.entry = .{ .symbol_name = "_start" };
    launcher_app.link_z_common_page_size = 0x10;
    launcher_app.link_z_max_page_size = 0x10;
    const install_launcher = b.addInstallArtifact(launcher_app, .{
        .dest_sub_path = "EFI/BOOT/LAUNCHER.ELF",
    });
    const launcher_step = b.step("launcher-elf", "Build launcher PIE ELF");
    launcher_step.dependOn(&install_launcher.step);

    const mouse_draw_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/mouse_draw.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    mouse_draw_mod.addImport("support_root", support_root_mod);
    mouse_draw_mod.strip = true;
    const mouse_draw_app = b.addExecutable(.{
        .name = "MDRAW",
        .root_module = mouse_draw_mod,
    });
    mouse_draw_app.pie = true;
    mouse_draw_app.entry = .{ .symbol_name = "_start" };
    mouse_draw_app.link_z_common_page_size = 0x10;
    mouse_draw_app.link_z_max_page_size = 0x10;
    const install_mouse_draw = b.addInstallArtifact(mouse_draw_app, .{
        .dest_sub_path = "EFI/BOOT/MDRAW.ELF",
    });
    const mouse_draw_step = b.step("mouse-draw-elf", "Build mouse draw PIE ELF");
    mouse_draw_step.dependOn(&install_mouse_draw.step);

    const compositor_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/compositor.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    compositor_mod.addImport("support_root", support_root_mod);
    compositor_mod.strip = true;
    const compositor_app = b.addExecutable(.{
        .name = "COMPOS",
        .root_module = compositor_mod,
    });
    compositor_app.pie = true;
    compositor_app.entry = .{ .symbol_name = "_start" };
    compositor_app.link_z_common_page_size = 0x10;
    compositor_app.link_z_max_page_size = 0x10;
    const install_compositor = b.addInstallArtifact(compositor_app, .{
        .dest_sub_path = "EFI/BOOT/COMPOS.ELF",
    });
    const compositor_step = b.step("compositor-elf", "Build compositor PIE ELF");
    compositor_step.dependOn(&install_compositor.step);

    const gpu_compositor_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/gpu_compositor.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    gpu_compositor_mod.addImport("support_root", support_root_mod);
    gpu_compositor_mod.strip = true;
    const gpu_compositor_app = b.addExecutable(.{
        .name = "GPUCOMP",
        .root_module = gpu_compositor_mod,
    });
    gpu_compositor_app.pie = true;
    gpu_compositor_app.entry = .{ .symbol_name = "_start" };
    gpu_compositor_app.link_z_common_page_size = 0x10;
    gpu_compositor_app.link_z_max_page_size = 0x10;
    const install_gpu_compositor = b.addInstallArtifact(gpu_compositor_app, .{
        .dest_sub_path = "EFI/BOOT/GPUCOMP.ELF",
    });
    const gpu_compositor_step = b.step("gpu-compositor-elf", "Build GPU compositor PIE ELF");
    gpu_compositor_step.dependOn(&install_gpu_compositor.step);

    const vfs_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/vfs.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    vfs_mod.addImport("support_root", support_root_mod);
    vfs_mod.strip = true;
    const vfs_app = b.addExecutable(.{
        .name = "VFS",
        .root_module = vfs_mod,
    });
    vfs_app.pie = true;
    vfs_app.entry = .{ .symbol_name = "_start" };
    vfs_app.link_z_common_page_size = 0x10;
    vfs_app.link_z_max_page_size = 0x10;
    const install_vfs = b.addInstallArtifact(vfs_app, .{
        .dest_sub_path = "EFI/BOOT/VFS.ELF",
    });
    const vfs_step = b.step("vfs-elf", "Build VFS PIE ELF");
    vfs_step.dependOn(&install_vfs.step);

    const virtio_blk_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/virtio_blk.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    virtio_blk_mod.addImport("support_root", support_root_mod);
    virtio_blk_mod.strip = true;
    const virtio_blk_app = b.addExecutable(.{
        .name = "VBLKDRV",
        .root_module = virtio_blk_mod,
    });
    virtio_blk_app.pie = true;
    virtio_blk_app.entry = .{ .symbol_name = "_start" };
    virtio_blk_app.link_z_common_page_size = 0x10;
    virtio_blk_app.link_z_max_page_size = 0x10;
    const install_virtio_blk = b.addInstallArtifact(virtio_blk_app, .{
        .dest_sub_path = "EFI/BOOT/VBLKDRV.ELF",
    });
    const virtio_blk_step = b.step("virtio-blk-elf", "Build virtio-blk PIE ELF");
    virtio_blk_step.dependOn(&install_virtio_blk.step);

    const bootstrap_fs_mod = b.createModule(.{
        .root_source_file = b.path("../bootstrap/programs/bootstrap_fs.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    bootstrap_fs_mod.addImport("support_root", support_root_mod);
    bootstrap_fs_mod.addImport("persistent_fs_layout", persistent_fs_layout_mod);
    bootstrap_fs_mod.strip = true;
    const bootstrap_fs_app = b.addExecutable(.{
        .name = "BOOTFSRV",
        .root_module = bootstrap_fs_mod,
    });
    bootstrap_fs_app.pie = true;
    bootstrap_fs_app.entry = .{ .symbol_name = "_start" };
    bootstrap_fs_app.link_z_common_page_size = 0x10;
    bootstrap_fs_app.link_z_max_page_size = 0x10;
    const install_bootstrap_fs = b.addInstallArtifact(bootstrap_fs_app, .{
        .dest_sub_path = "EFI/BOOT/BOOTFSRV.ELF",
    });
    const bootstrap_fs_step = b.step("bootstrap-fs-elf", "Build bootstrap fs PIE ELF");
    bootstrap_fs_step.dependOn(&install_bootstrap_fs.step);

    const block_demo_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/block_demo.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    block_demo_mod.addImport("support_root", support_root_mod);
    block_demo_mod.strip = true;
    const block_demo_app = b.addExecutable(.{
        .name = "BLKDEMO",
        .root_module = block_demo_mod,
    });
    block_demo_app.pie = true;
    block_demo_app.entry = .{ .symbol_name = "_start" };
    block_demo_app.link_z_common_page_size = 0x10;
    block_demo_app.link_z_max_page_size = 0x10;
    const install_block_demo = b.addInstallArtifact(block_demo_app, .{
        .dest_sub_path = "EFI/BOOT/BLKDEMO.ELF",
    });
    const block_demo_step = b.step("block-demo-elf", "Build block demo PIE ELF");
    block_demo_step.dependOn(&install_block_demo.step);

    const persistent_fs_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/persistent_fs.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    persistent_fs_mod.addImport("support_root", support_root_mod);
    persistent_fs_mod.addImport("persistent_fs_layout", persistent_fs_layout_mod);
    persistent_fs_mod.strip = true;
    const persistent_fs_app = b.addExecutable(.{
        .name = "PERSFS",
        .root_module = persistent_fs_mod,
    });
    persistent_fs_app.pie = true;
    persistent_fs_app.entry = .{ .symbol_name = "_start" };
    persistent_fs_app.link_z_common_page_size = 0x10;
    persistent_fs_app.link_z_max_page_size = 0x10;
    const install_persistent_fs = b.addInstallArtifact(persistent_fs_app, .{
        .dest_sub_path = "EFI/BOOT/PERSFS.ELF",
    });
    const persistent_fs_step = b.step("persistent-fs-elf", "Build persistent fs PIE ELF");
    persistent_fs_step.dependOn(&install_persistent_fs.step);

    const pie_user_mod = b.createModule(.{
        .root_source_file = b.path("../userland/programs/pie_user.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    pie_user_mod.addImport("support_root", support_root_mod);
    pie_user_mod.strip = true;
    pie_user_mod.addIncludePath(b.path("../user/libcapc"));
    pie_user_mod.addCSourceFiles(.{
        .root = b.path("../user/libcapc"),
        .files = &.{
            "capc.c",
            "cap_errno.c",
        },
        .flags = &.{
            "-ffreestanding",
            "-fno-stack-protector",
        },
    });
    const pie_user_app = b.addExecutable(.{
        .name = "USERAPP",
        .root_module = pie_user_mod,
    });
    pie_user_app.pie = true;
    pie_user_app.entry = .{ .symbol_name = "_start" };
    pie_user_app.link_z_common_page_size = 0x10;
    pie_user_app.link_z_max_page_size = 0x10;
    const install_pie_user = b.addInstallArtifact(pie_user_app, .{
        .dest_sub_path = "EFI/BOOT/USERAPP.ELF",
    });
    const pie_user_step = b.step("pie-user-elf", "Build pie_user PIE ELF");
    pie_user_step.dependOn(&install_pie_user.step);

    const bootfs_builder_mod = b.createModule(.{
        .root_source_file = b.path("../tools/bootfs_builder.zig"),
        .target = b.graph.host,
        .optimize = optimize,
    });
    const bootfs_builder_app = b.addExecutable(.{
        .name = "bootfs_builder",
        .root_module = bootfs_builder_mod,
    });
    const bootfs_builder_run = b.addRunArtifact(bootfs_builder_app);
    const bootfs_image_out = bootfs_builder_run.addOutputFileArg("BOOTFS.IMG");
    bootfs_builder_run.addArg("/boot/startup_manifest.txt");
    bootfs_builder_run.addFileArg(b.path("../bootstrap/bootfs/startup_manifest.txt"));
    bootfs_builder_run.addArg("/bin/bootstrap_fs.elf");
    bootfs_builder_run.addFileArg(bootstrap_fs_app.getEmittedBin());
    bootfs_builder_run.addArg("/bin/shell.elf");
    bootfs_builder_run.addFileArg(shell_app.getEmittedBin());
    const install_bootfs_image = b.addInstallBinFile(bootfs_image_out, "EFI/BOOT/BOOTFS.IMG");
    const bootfs_step = b.step("bootfs-img", "Build bootfs image");
    bootfs_step.dependOn(&install_bootfs_image.step);

    const rootfs_host_mod = b.createModule(.{
        .root_source_file = b.path("../tools/rootfs_host.zig"),
        .target = b.graph.host,
        .optimize = optimize,
    });
    rootfs_host_mod.addImport("persistent_fs_layout", persistent_fs_layout_mod);

    const rootfs_put_mod = b.createModule(.{
        .root_source_file = b.path("../tools/rootfs_put.zig"),
        .target = b.graph.host,
        .optimize = optimize,
    });
    rootfs_put_mod.addImport("rootfs_host", rootfs_host_mod);
    const rootfs_put_app = b.addExecutable(.{
        .name = "rootfs_put",
        .root_module = rootfs_put_mod,
    });
    const install_rootfs_put = b.addInstallArtifact(rootfs_put_app, .{});
    const rootfs_put_step = b.step("rootfs-put-tool", "Build host-side rootfs_put tool");
    rootfs_put_step.dependOn(&install_rootfs_put.step);

    const rootfs_builder_mod = b.createModule(.{
        .root_source_file = b.path("../tools/rootfs_builder.zig"),
        .target = b.graph.host,
        .optimize = optimize,
    });
    rootfs_builder_mod.addImport("rootfs_host", rootfs_host_mod);
    const rootfs_builder_app = b.addExecutable(.{
        .name = "rootfs_builder",
        .root_module = rootfs_builder_mod,
    });
    const install_rootfs_builder = b.addInstallArtifact(rootfs_builder_app, .{});
    const rootfs_builder_step = b.step("rootfs-builder-tool", "Build host-side rootfs_builder tool");
    rootfs_builder_step.dependOn(&install_rootfs_builder.step);

    const esp_builder_mod = b.createModule(.{
        .root_source_file = b.path("../tools/esp_builder.zig"),
        .target = b.graph.host,
        .optimize = optimize,
    });
    esp_builder_mod.addImport("rootfs_host", rootfs_host_mod);
    const esp_builder_app = b.addExecutable(.{
        .name = "esp_builder",
        .root_module = esp_builder_mod,
    });
    const install_esp_builder = b.addInstallArtifact(esp_builder_app, .{});
    const esp_builder_step = b.step("esp-builder-tool", "Build host-side esp_builder tool");
    esp_builder_step.dependOn(&install_esp_builder.step);

    const capc_hello_cmd = b.addSystemCommand(&[_][]const u8{
        b.graph.zig_exe,
        "cc",
        "-target",
        "x86_64-freestanding-none",
        "-nostdlib",
        "-ffreestanding",
        "-fno-stack-protector",
        "-fPIE",
        "-I",
        "../user/libcapc",
        "../user/libcapc/crt0.S",
        "../user/libcapc/capc.c",
        "../user/libcapc/cap_errno.c",
        "../user/libcapc/hello_capc.c",
        "-Wl,-e,_start",
        "-Wl,-z,common-page-size=0x10",
        "-Wl,-z,max-page-size=0x10",
    });
    capc_hello_cmd.addArg("-o");
    const capc_hello_out = capc_hello_cmd.addOutputFileArg("CAPCHEL.ELF");
    const install_capc_hello = b.addInstallFile(capc_hello_out, "EFI/BOOT/CAPCHEL.ELF");
    const capc_hello_step = b.step("capc-hello-elf", "Build libcapc C hello PIE ELF");
    capc_hello_step.dependOn(&install_capc_hello.step);

    const install_efi = b.addInstallArtifact(efi_app, .{
        .dest_sub_path = "EFI/BOOT/BOOTX64.EFI",
    });
    install_efi.step.dependOn(&install_init.step);
    install_efi.step.dependOn(&install_shell.step);
    install_efi.step.dependOn(&install_bootstrap_fs.step);
    install_efi.step.dependOn(&install_virtio_blk.step);
    install_efi.step.dependOn(&install_persistent_fs.step);
    install_efi.step.dependOn(&install_pie_user.step);
    install_efi.step.dependOn(&install_bootfs_image.step);
    install_efi.step.dependOn(&install_rootfs_put.step);
    install_efi.step.dependOn(&install_rootfs_builder.step);
    install_efi.step.dependOn(&install_esp_builder.step);
    const efi_step = b.step("efi", "Build UEFI kernel application");
    efi_step.dependOn(&install_efi.step);
}
