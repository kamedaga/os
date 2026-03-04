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
    });
    user_mod.strip = true;
    const user_app = b.addExecutable(.{
        .name = "USERAPP",
        .root_module = user_mod,
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

    const mouse_draw_mod = b.createModule(.{
        .root_source_file = b.path("user_programs/mouse_draw.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
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

    const framebuffer_server_mod = b.createModule(.{
        .root_source_file = b.path("user_programs/framebuffer_server.zig"),
        .target = user_target,
        .optimize = .ReleaseSmall,
        .code_model = .small,
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

    const install_efi = b.addInstallArtifact(efi_app, .{
        .dest_sub_path = "EFI/BOOT/BOOTX64.EFI",
    });
    install_efi.step.dependOn(&install_user.step);
    install_efi.step.dependOn(&install_draw_client.step);
    install_efi.step.dependOn(&install_boot_log_console.step);
    install_efi.step.dependOn(&install_mouse_driver.step);
    install_efi.step.dependOn(&install_mouse_draw.step);
    install_efi.step.dependOn(&install_compositor.step);
    install_efi.step.dependOn(&install_framebuffer_server.step);
    const efi_step = b.step("efi", "Build UEFI kernel application");
    efi_step.dependOn(&install_efi.step);
}
