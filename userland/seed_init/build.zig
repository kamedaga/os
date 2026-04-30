const std = @import("std");

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{});
    const target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .freestanding,
        .abi = .none,
    });

    const persistent_fs_layout_mod = b.createModule(.{
        .root_source_file = b.path("../programs/abi/persistent_fs_layout.zig"),
    });
    const abi_root_mod = b.createModule(.{
        .root_source_file = b.path("../programs/abi/abi_root.zig"),
        .target = target,
        .optimize = optimize,
        .code_model = .small,
        .red_zone = false,
    });
    abi_root_mod.addImport("persistent_fs_layout", persistent_fs_layout_mod);

    const seed_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        .code_model = .small,
        .red_zone = false,
    });
    seed_mod.addImport("abi_root", abi_root_mod);
    seed_mod.strip = true;

    const seed_app = b.addExecutable(.{
        .name = "seed",
        .root_module = seed_mod,
    });
    seed_app.pie = true;
    seed_app.entry = .{ .symbol_name = "_start" };
    seed_app.link_z_common_page_size = 0x10;
    seed_app.link_z_max_page_size = 0x10;

    const install_seed = b.addInstallArtifact(seed_app, .{
        .dest_sub_path = "seed.elf",
    });
    const seed_step = b.step("seed-elf", "Build seed init PIE ELF");
    seed_step.dependOn(&install_seed.step);
}




