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
        .optimize = .ReleaseSmall,
        .code_model = .small,
    });
    const efi_app = b.addExecutable(.{
        .name = "BOOTX64",
        .root_module = efi_mod,
    });

    const user_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .freestanding,
        .abi = .none,
    });
    const user_mod = b.createModule(.{
        .root_source_file = b.path("user_programs/pie_user.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    user_mod.strip = true;
    const user_app = b.addExecutable(.{
        .name = "USERAPP",
        .root_module = user_mod,
    });
    user_app.addIncludePath(b.path("../user/libcapc"));
    user_app.addCSourceFile(.{
        .file = b.path("../user/libcapc/capc.c"),
        .flags = &.{},
    });
    user_app.addCSourceFile(.{
        .file = b.path("../user/libcapc/cap_errno.c"),
        .flags = &.{},
    });
    user_app.pie = true;
    user_app.entry = .{ .symbol_name = "_start" };
    user_app.link_z_common_page_size = 0x10;
    user_app.link_z_max_page_size = 0x10;
    const install_user = b.addInstallArtifact(user_app, .{
        .dest_sub_path = "EFI/BOOT/USERAPP.ELF",
    });
    const user_step = b.step("user-elf", "Build PIE user ELF");
    user_step.dependOn(&install_user.step);

    const draw_client_mod = b.createModule(.{
        .root_source_file = b.path("user_programs/draw_client.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    draw_client_mod.strip = true;
    const draw_client_app = b.addExecutable(.{
        .name = "DRAWCLI",
        .root_module = draw_client_mod,
    });
    draw_client_app.pie = true;
    draw_client_app.entry = .{ .symbol_name = "_start" };
    draw_client_app.link_z_common_page_size = 0x10;
    draw_client_app.link_z_max_page_size = 0x10;
    const install_draw_client = b.addInstallArtifact(draw_client_app, .{
        .dest_sub_path = "EFI/BOOT/DRAWCLI.ELF",
    });
    const draw_client_step = b.step("draw-client-elf", "Build draw client PIE ELF");
    draw_client_step.dependOn(&install_draw_client.step);

    const boot_log_console_mod = b.createModule(.{
        .root_source_file = b.path("user_programs/boot_log_console.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    boot_log_console_mod.strip = true;
    const boot_log_console_app = b.addExecutable(.{
        .name = "BOOTLOG",
        .root_module = boot_log_console_mod,
    });
    boot_log_console_app.pie = true;
    boot_log_console_app.entry = .{ .symbol_name = "_start" };
    boot_log_console_app.link_z_common_page_size = 0x10;
    boot_log_console_app.link_z_max_page_size = 0x10;
    const install_boot_log_console = b.addInstallArtifact(boot_log_console_app, .{
        .dest_sub_path = "EFI/BOOT/BOOTLOG.ELF",
    });
    const boot_log_console_step = b.step("boot-log-console-elf", "Build boot log console PIE ELF");
    boot_log_console_step.dependOn(&install_boot_log_console.step);

    const mouse_driver_mod = b.createModule(.{
        .root_source_file = b.path("user_programs/mouse_driver.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
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
        .root_source_file = b.path("user_programs/keyboard_driver.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
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
        .root_source_file = b.path("user_programs/bootlog_sender.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
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
        .root_source_file = b.path("user_programs/mouse_button_demo.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
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

    const keyboard_ascii_demo_mod = b.createModule(.{
        .root_source_file = b.path("user_programs/keyboard_ascii_demo.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    keyboard_ascii_demo_mod.strip = true;
    const keyboard_ascii_demo_app = b.addExecutable(.{
        .name = "KEYBDEMO",
        .root_module = keyboard_ascii_demo_mod,
    });
    keyboard_ascii_demo_app.pie = true;
    keyboard_ascii_demo_app.entry = .{ .symbol_name = "_start" };
    keyboard_ascii_demo_app.link_z_common_page_size = 0x10;
    keyboard_ascii_demo_app.link_z_max_page_size = 0x10;
    const install_keyboard_ascii_demo = b.addInstallArtifact(keyboard_ascii_demo_app, .{
        .dest_sub_path = "EFI/BOOT/KEYBDEMO.ELF",
    });
    const keyboard_ascii_demo_step = b.step("keyboard-ascii-demo-elf", "Build keyboard ASCII demo PIE ELF");
    keyboard_ascii_demo_step.dependOn(&install_keyboard_ascii_demo.step);

    const terminal_window_mod = b.createModule(.{
        .root_source_file = b.path("user_programs/terminal_window.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
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

    const mouse_draw_mod = b.createModule(.{
        .root_source_file = b.path("user_programs/mouse_draw.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
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
        .root_source_file = b.path("user_programs/compositor.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
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
        .root_source_file = b.path("user_programs/gpu_compositor.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
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

    const framebuffer_server_mod = b.createModule(.{
        .root_source_file = b.path("user_programs/framebuffer_server.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
        .red_zone = false,
    });
    framebuffer_server_mod.strip = true;
    const framebuffer_server_app = b.addExecutable(.{
        .name = "FBSRV",
        .root_module = framebuffer_server_mod,
    });
    framebuffer_server_app.pie = true;
    framebuffer_server_app.entry = .{ .symbol_name = "_start" };
    framebuffer_server_app.link_z_common_page_size = 0x10;
    framebuffer_server_app.link_z_max_page_size = 0x10;
    const install_framebuffer_server = b.addInstallArtifact(framebuffer_server_app, .{
        .dest_sub_path = "EFI/BOOT/FBSRV.ELF",
    });
    const framebuffer_server_step = b.step("framebuffer-server-elf", "Build framebuffer server PIE ELF");
    framebuffer_server_step.dependOn(&install_framebuffer_server.step);

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
    install_efi.step.dependOn(&install_user.step);
    install_efi.step.dependOn(&install_draw_client.step);
    install_efi.step.dependOn(&install_boot_log_console.step);
    install_efi.step.dependOn(&install_mouse_driver.step);
    install_efi.step.dependOn(&install_keyboard_driver.step);
    install_efi.step.dependOn(&install_bootlog_sender.step);
    install_efi.step.dependOn(&install_mouse_button_demo.step);
    install_efi.step.dependOn(&install_keyboard_ascii_demo.step);
    install_efi.step.dependOn(&install_terminal_window.step);
    install_efi.step.dependOn(&install_mouse_draw.step);
    install_efi.step.dependOn(&install_compositor.step);
    install_efi.step.dependOn(&install_gpu_compositor.step);
    install_efi.step.dependOn(&install_framebuffer_server.step);
    install_efi.step.dependOn(&install_capc_hello.step);
    const efi_step = b.step("efi", "Build UEFI kernel application");
    efi_step.dependOn(&install_efi.step);
}
