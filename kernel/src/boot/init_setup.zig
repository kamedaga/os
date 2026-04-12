/// Init process bootstrap resource setup.
/// Sets up the init process's bootstrap descriptor page, spawn pages, service
/// registry, and virtio device capability grants.
const std = @import("std");
const kernel = @import("../kernel.zig");
const boot_abi = @import("abi.zig");
const boot_static = @import("main_static.zig");
const init_bootstrap_layout = @import("init_bootstrap_layout.zig");
const process_factory = @import("process_factory.zig");
const uefi_services = @import("uefi_services.zig");
const halt = @import("../halt.zig");
const log_util = @import("../log_util.zig");

const init_bootstrap_abi = boot_abi.init_bootstrap_abi;
const service_registry_abi = boot_abi.service_registry_abi;
const boot_manifest_abi = boot_abi.boot_manifest_abi;


// ---------------------------------------------------------------------------
// Driver device config types (kept here since they are only used during init setup)
// ---------------------------------------------------------------------------

pub const MmioPageWithOffset = struct {
    page_paddr: u64,
    page_offset: u64,
};

pub const MouseDriverConfig = struct {
    common: MmioPageWithOffset,
    notify: MmioPageWithOffset,
    isr: MmioPageWithOffset,
    device: MmioPageWithOffset,
    notify_off_multiplier: u64,
    queue_paddr0: u64 = 0,
    queue_paddr1: u64 = 0,
};

pub const VirtioBlkDriverConfig = struct {
    common: MmioPageWithOffset,
    notify: MmioPageWithOffset,
    isr: MmioPageWithOffset,
    device: MmioPageWithOffset,
    notify_off_multiplier: u64,
    capacity_sectors: u64,
    logical_block_size: u64,
};

pub const DetectedInputBootstrap = struct {
    descriptor: init_bootstrap_abi.InputDeviceDescriptor,
    config: MouseDriverConfig,
};

pub const DetectedBlockBootstrap = struct {
    descriptor: init_bootstrap_abi.BlockDeviceDescriptor,
    config: VirtioBlkDriverConfig,
};

pub const BootFsImageSetup = struct {
    first_page_paddr: u64,
    size_bytes: u64,
    page_count: usize,
    page_paddrs: [init_bootstrap_abi.max_boot_archive_pages]u64,
};

pub const KernelBackedInitSpawnPage = struct {
    descriptor: init_bootstrap_abi.SpawnPageDescriptor,
    page: kernel.PageCapability,
};

// ---------------------------------------------------------------------------
// Halt helpers (scoped to init bootstrap context)
// ---------------------------------------------------------------------------

fn haltInitBootstrapDescriptor(message: []const u8) noreturn {
    halt.haltWithLabelMessage("init bootstrap descriptor invalid:", message);
}

fn haltInitInputBootstrapError(label: []const u8, step: []const u8, err: anyerror) noreturn {
    halt.haltWithStepError("init ", label, step, err);
}

fn haltInitBlockBootstrapError(label: []const u8, step: []const u8, err: anyerror) noreturn {
    halt.haltWithStepError("init ", label, step, err);
}

// ---------------------------------------------------------------------------
// Spawn page descriptor helpers
// ---------------------------------------------------------------------------

fn spawnPageChildWritable(descriptor: init_bootstrap_abi.SpawnPageDescriptor) bool {
    return (descriptor.spawn_flags & boot_abi.process_abi.spawn_flag_bootstrap_page_writable) != 0;
}

fn spawnPageInitWritable(descriptor: init_bootstrap_abi.SpawnPageDescriptor) bool {
    return (descriptor.flags & init_bootstrap_abi.spawn_page_flag_init_writable) != 0;
}

fn spawnPageMirrorWritable(descriptor: init_bootstrap_abi.SpawnPageDescriptor) bool {
    return (descriptor.flags & init_bootstrap_abi.spawn_page_flag_mirror_writable) != 0;
}

fn initSpawnPageLabel(_: init_bootstrap_abi.SpawnPageDescriptor) []const u8 {
    return "spawn page";
}

// ---------------------------------------------------------------------------
// Input/block device descriptor helpers
// ---------------------------------------------------------------------------

fn inputDeviceKindFromDescriptor(descriptor: init_bootstrap_abi.InputDeviceDescriptor) init_bootstrap_abi.InputDeviceKind {
    return @enumFromInt(descriptor.kind);
}

fn inputDeviceLabel(kind: init_bootstrap_abi.InputDeviceKind) []const u8 {
    return switch (kind) {
        .pointer => "pointer input",
        .keyboard => "keyboard input",
    };
}

fn inputDeviceConfigPageLabel(kind: init_bootstrap_abi.InputDeviceKind) []const u8 {
    return switch (kind) {
        .pointer => "pointer input config page",
        .keyboard => "keyboard input config page",
    };
}

// ---------------------------------------------------------------------------
// Bootfs image mapping
// ---------------------------------------------------------------------------

pub fn mapBootFsImageIntoProcessOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    role_label: []const u8,
    base_va: u64,
    image: []const u8,
    free_list: *kernel.FreePageList,
) BootFsImageSetup {
    const page_count = std.math.divCeil(usize, image.len, 4096) catch unreachable;
    if (page_count > init_bootstrap_abi.max_boot_archive_pages) {
        halt.haltWithLabelMessage(role_label, " bootfs image too large for bootstrap descriptor");
    }
    var first_page_paddr: u64 = 0;
    var page_paddrs = [_]u64{0} ** init_bootstrap_abi.max_boot_archive_pages;
    var copied: usize = 0;
    var page_index: usize = 0;
    while (page_index < page_count) : (page_index += 1) {
        const page = process_factory.allocPageForProcessOrHalt(state, principal, role_label, "bootfs image page", free_list);
        if (first_page_paddr == 0) first_page_paddr = page.paddr;
        page_paddrs[page_index] = page.paddr;
        const dst: [*]u8 = @ptrFromInt(page.paddr);
        @memset(dst[0..4096], 0);
        const remaining = image.len - copied;
        const chunk_len: usize = if (remaining > 4096) 4096 else remaining;
        @memcpy(dst[0..chunk_len], image[copied .. copied + chunk_len]);
        process_factory.mapUserPageOrHalt(principal, base_va + @as(u64, @intCast(page_index)) * 4096, page, false, role_label, "bootfs image page");
        copied += chunk_len;
    }
    return .{
        .first_page_paddr = first_page_paddr,
        .size_bytes = image.len,
        .page_count = page_count,
        .page_paddrs = page_paddrs,
    };
}

// ---------------------------------------------------------------------------
// Kernel-backed spawn pages
// ---------------------------------------------------------------------------

pub fn allocKernelBackedInitSpawnPages(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    free_list: *kernel.FreePageList,
) [init_bootstrap_layout.builtin_spawn_pages.len]?KernelBackedInitSpawnPage {
    var pages: [init_bootstrap_layout.builtin_spawn_pages.len]?KernelBackedInitSpawnPage =
        [_]?KernelBackedInitSpawnPage{null} ** init_bootstrap_layout.builtin_spawn_pages.len;
    inline for (init_bootstrap_layout.builtin_spawn_pages, 0..) |descriptor, idx| {
        if ((descriptor.flags & init_bootstrap_abi.spawn_page_flag_kernel_backed) == 0) continue;
        const page = process_factory.allocAndMapOwnedPageForProcessOrHalt(
            state,
            init_process_principal,
            "init",
            initSpawnPageLabel(descriptor),
            descriptor.source_va,
            spawnPageInitWritable(descriptor),
            free_list,
        );
        pages[idx] = .{ .descriptor = descriptor, .page = page };
    }
    return pages;
}

fn findKernelBackedInitSpawnPage(
    pages: []const ?KernelBackedInitSpawnPage,
    source_va: u64,
) ?KernelBackedInitSpawnPage {
    for (pages) |entry| {
        const page = entry orelse continue;
        if (page.descriptor.source_va == source_va) return page;
    }
    return null;
}

fn mirrorKernelBackedInitSpawnPages(
    state: *kernel.KernelState,
    boot_display_process: kernel.PrincipalId,
    pages: []const ?KernelBackedInitSpawnPage,
) void {
    inline for (init_bootstrap_layout.builtin_spawn_pages) |descriptor| {
        if ((descriptor.flags & init_bootstrap_abi.spawn_page_flag_mirror_to_boot_display) == 0) continue;
        const page = findKernelBackedInitSpawnPage(pages, descriptor.source_va) orelse
            haltInitBootstrapDescriptor("missing kernel-backed spawn page for mirror");
        process_factory.installAndMapPageForProcessOrHalt(
            state,
            boot_display_process,
            page.page,
            descriptor.target_va,
            spawnPageMirrorWritable(descriptor),
            "compositor",
            initSpawnPageLabel(descriptor),
        );
    }
}

// ---------------------------------------------------------------------------
// Service registry
// ---------------------------------------------------------------------------

const InitServiceDescriptorSet = struct {
    count: usize = 0,
    descriptors: [service_registry_abi.max_entries]service_registry_abi.ServiceEntry =
        [_]service_registry_abi.ServiceEntry{.{
            .kind = 0,
            .process_slot = 0,
            .endpoint_id = 0,
            .flags = 0,
        }} ** service_registry_abi.max_entries,
};

fn buildInitServiceDescriptors(boot_display_process: kernel.PrincipalId) InitServiceDescriptorSet {
    var services = InitServiceDescriptorSet{};
    const process_index = kernel.processIndexFromPrincipal(boot_display_process) orelse
        haltInitBootstrapDescriptor("boot display principal index missing");
    services.descriptors[0] = .{
        .kind = @intFromEnum(service_registry_abi.ServiceKind.window),
        .process_slot = @intCast(process_index),
        .endpoint_id = service_registry_abi.init_window_service_endpoint_id,
        .flags = 0,
    };
    services.count = 1;
    return services;
}

fn publishInitServiceRegistryPage(
    pages: []const ?KernelBackedInitSpawnPage,
    services: []const service_registry_abi.ServiceEntry,
) void {
    const page = findKernelBackedInitSpawnPage(pages, init_bootstrap_layout.sourceVa(.window_service_config)) orelse
        haltInitBootstrapDescriptor("missing window service config page");
    service_registry_abi.initPage(page.page.paddr);
    for (services) |descriptor| {
        const kind = std.meta.intToEnum(service_registry_abi.ServiceKind, descriptor.kind) catch
            haltInitBootstrapDescriptor("invalid init service descriptor kind");
        service_registry_abi.addService(
            page.page.paddr,
            kind,
            descriptor.process_slot,
            descriptor.endpoint_id,
        );
    }
}

// ---------------------------------------------------------------------------
// Bootstrap config/descriptor page publishing
// ---------------------------------------------------------------------------

fn publishInitBootstrapConfigPage(user_page_paddr: u64, descriptor_page_va: u64) void {
    const page: *volatile init_bootstrap_abi.ConfigPage = @ptrFromInt(user_page_paddr);
    page.magic = init_bootstrap_abi.config_magic;
    page.version = init_bootstrap_abi.config_version;
    page.descriptor_page_va = descriptor_page_va;
    page.reserved0 = 0;
}

pub fn publishInitBootstrapDescriptorPage(
    user_page_paddr: u64,
    input_devices: []const ?DetectedInputBootstrap,
    block_device: ?DetectedBlockBootstrap,
    bootfs_setup: BootFsImageSetup,
    framebuffer_info: ?uefi_services.FramebufferInfo,
) void {
    const page: *volatile init_bootstrap_abi.DescriptorPage = @ptrFromInt(user_page_paddr);
    page.magic = init_bootstrap_abi.magic;
    page.version = init_bootstrap_abi.version;
    page.spawn_page_count = init_bootstrap_layout.builtin_spawn_pages.len;
    page.boot_image_count = boot_manifest_abi.builtin_boot_images.len;
    page.bootfs_archive = .{
        .flags = init_bootstrap_abi.boot_archive_flag_present,
        .image_va = init_bootstrap_layout.bootfs_image_va,
        .size_bytes = bootfs_setup.size_bytes,
        .page_count = bootfs_setup.page_count,
    };
    page.primary_display = .{ .flags = 0, .width = 0, .height = 0, .pitch = 0 };
    if (framebuffer_info) |info| {
        page.primary_display.flags = init_bootstrap_abi.display_flag_present;
        page.primary_display.width = info.width;
        page.primary_display.height = info.height;
        page.primary_display.pitch = info.pixels_per_scan_line;
    }

    var i: usize = 0;
    while (i < init_bootstrap_abi.max_spawn_page_descriptors) : (i += 1) {
        page.spawn_pages[i] = .{ .kind = 0, .subject = 0, .flags = 0, .source_va = 0, .target_va = 0, .spawn_flags = 0 };
    }
    inline for (init_bootstrap_layout.builtin_spawn_pages, 0..) |descriptor, idx| {
        page.spawn_pages[idx] = descriptor;
    }

    var boot_image_idx: usize = 0;
    while (boot_image_idx < boot_manifest_abi.max_boot_image_descriptors) : (boot_image_idx += 1) {
        page.boot_images[boot_image_idx] = .{ .kind = 0, .payload_kind = 0, .flags = 0 };
    }
    var bootfs_page_idx: usize = 0;
    while (bootfs_page_idx < init_bootstrap_abi.max_boot_archive_pages) : (bootfs_page_idx += 1) {
        page.bootfs_page_paddrs[bootfs_page_idx] = 0;
    }
    var page_copy_idx: usize = 0;
    while (page_copy_idx < bootfs_setup.page_count) : (page_copy_idx += 1) {
        page.bootfs_page_paddrs[page_copy_idx] = bootfs_setup.page_paddrs[page_copy_idx];
    }
    inline for (boot_manifest_abi.builtin_boot_images, 0..) |descriptor, idx| {
        var updated = descriptor;
        const kind: boot_manifest_abi.ImageKind = @enumFromInt(updated.kind);
        const present = switch (kind) {
            .boot_log_console => true,
            .vfs => false,
            .init_app => true,
            .bootfs_image => true,
        };
        if (present) {
            updated.flags |= boot_manifest_abi.image_flag_present | boot_manifest_abi.image_flag_kernel_loaded;
        }
        page.boot_images[idx] = updated;
    }

    var input_count: usize = 0;
    while (input_count < init_bootstrap_abi.max_input_device_descriptors) : (input_count += 1) {
        page.input_devices[input_count] = .{
            .kind = 0,
            .flags = 0,
            .config_source_va = 0,
            .config_target_va = 0,
            .config_spawn_flags = 0,
            .common_page_paddr = 0,
            .notify_page_paddr = 0,
            .isr_page_paddr = 0,
            .device_page_paddr = 0,
            .common_page_offset = 0,
            .notify_page_offset = 0,
            .isr_page_offset = 0,
            .device_page_offset = 0,
            .notify_off_multiplier = 0,
            .init_queue_submit_token = 0,
            .init_queue_notify_token = 0,
        };
    }
    input_count = 0;
    for (input_devices) |entry| {
        const device = entry orelse continue;
        if (input_count >= init_bootstrap_abi.max_input_device_descriptors) break;
        var descriptor = device.descriptor;
        descriptor.flags |= init_bootstrap_abi.input_device_flag_present;
        page.input_devices[input_count] = descriptor;
        input_count += 1;
    }
    page.input_device_count = input_count;

    var block_idx: usize = 0;
    while (block_idx < init_bootstrap_abi.max_block_device_descriptors) : (block_idx += 1) {
        page.block_devices[block_idx] = .{
            .kind = 0,
            .flags = 0,
            .config_source_va = 0,
            .config_target_va = 0,
            .config_spawn_flags = 0,
            .common_page_paddr = 0,
            .notify_page_paddr = 0,
            .isr_page_paddr = 0,
            .device_page_paddr = 0,
            .common_page_offset = 0,
            .notify_page_offset = 0,
            .isr_page_offset = 0,
            .device_page_offset = 0,
            .notify_off_multiplier = 0,
            .capacity_sectors = 0,
            .logical_block_size = 0,
            .init_queue_submit_token = 0,
            .init_queue_notify_token = 0,
            .init_root_token = 0,
        };
    }
    if (block_device) |device| {
        var descriptor = device.descriptor;
        descriptor.flags |= init_bootstrap_abi.block_device_flag_present;
        page.block_devices[0] = descriptor;
        page.block_device_count = 1;
    } else {
        page.block_device_count = 0;
    }
}

// ---------------------------------------------------------------------------
// Input driver bootstrap for init
// ---------------------------------------------------------------------------

fn installInitInputMmioCapOrHalt(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    label: []const u8,
    cap_label: []const u8,
    paddr: u64,
    rights: kernel.Rights,
) void {
    state.installCap(init_process_principal, paddr, rights) catch |err| {
        haltInitInputBootstrapError(label, cap_label, err);
    };
}

fn grantInitInputQueueTokenOrHalt(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    label: []const u8,
    step_label: []const u8,
    submit: bool,
    notify: bool,
) u64 {
    return state.queueCapGrantStage2(init_process_principal, .virtio_input, 0, submit, notify) catch |err| {
        haltInitInputBootstrapError(label, step_label, err);
    };
}

pub fn setupInputDriverBootstrapForInit(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    device: DetectedInputBootstrap,
    free_list: *kernel.FreePageList,
) DetectedInputBootstrap {
    const kind = inputDeviceKindFromDescriptor(device.descriptor);
    const label = inputDeviceLabel(kind);
    _ = process_factory.allocAndMapOwnedPageForProcessOrHalt(
        state,
        init_process_principal,
        "init",
        inputDeviceConfigPageLabel(kind),
        device.descriptor.config_source_va,
        true,
        free_list,
    );
    const mmio_rw_rights = kernel.Rights{ .cpu_read = true, .cpu_write = true, .dma = false, .grant = true };
    const mmio_ro_rights = kernel.Rights{ .cpu_read = true, .cpu_write = false, .dma = false, .grant = true };
    const cfg = device.config;
    installInitInputMmioCapOrHalt(state, init_process_principal, label, "install common cap", cfg.common.page_paddr, mmio_rw_rights);
    installInitInputMmioCapOrHalt(state, init_process_principal, label, "install notify cap", cfg.notify.page_paddr, mmio_rw_rights);
    if (cfg.isr.page_paddr != 0) installInitInputMmioCapOrHalt(state, init_process_principal, label, "install isr cap", cfg.isr.page_paddr, mmio_ro_rights);
    if (cfg.device.page_paddr != 0) installInitInputMmioCapOrHalt(state, init_process_principal, label, "install device cap", cfg.device.page_paddr, mmio_ro_rights);
    const init_submit_token = grantInitInputQueueTokenOrHalt(state, init_process_principal, label, "queue submit grant", true, false);
    const init_notify_token = grantInitInputQueueTokenOrHalt(state, init_process_principal, label, "queue notify grant", false, true);
    var updated = device;
    updated.descriptor.common_page_paddr = cfg.common.page_paddr;
    updated.descriptor.notify_page_paddr = cfg.notify.page_paddr;
    updated.descriptor.isr_page_paddr = cfg.isr.page_paddr;
    updated.descriptor.device_page_paddr = cfg.device.page_paddr;
    updated.descriptor.common_page_offset = cfg.common.page_offset;
    updated.descriptor.notify_page_offset = cfg.notify.page_offset;
    updated.descriptor.isr_page_offset = cfg.isr.page_offset;
    updated.descriptor.device_page_offset = cfg.device.page_offset;
    updated.descriptor.notify_off_multiplier = cfg.notify_off_multiplier;
    updated.descriptor.init_queue_submit_token = init_submit_token;
    updated.descriptor.init_queue_notify_token = init_notify_token;
    return updated;
}

// ---------------------------------------------------------------------------
// Block driver bootstrap for init
// ---------------------------------------------------------------------------

fn installInitBlockMmioCapOrHalt(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    label: []const u8,
    cap_label: []const u8,
    paddr: u64,
    rights: kernel.Rights,
) void {
    state.installCap(init_process_principal, paddr, rights) catch |err| {
        haltInitBlockBootstrapError(label, cap_label, err);
    };
}

fn grantInitBlockQueueTokenOrHalt(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    label: []const u8,
    step_label: []const u8,
    submit: bool,
    notify: bool,
) u64 {
    return state.queueCapGrantStage2(init_process_principal, .virtio_blk, 0, submit, notify) catch |err| {
        haltInitBlockBootstrapError(label, step_label, err);
    };
}

pub fn setupBlockDriverBootstrapForInit(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    device: DetectedBlockBootstrap,
    free_list: *kernel.FreePageList,
) DetectedBlockBootstrap {
    const label = "block device";
    _ = process_factory.allocAndMapOwnedPageForProcessOrHalt(
        state,
        init_process_principal,
        "init",
        "block device config page",
        device.descriptor.config_source_va,
        true,
        free_list,
    );
    const mmio_rw_rights = kernel.Rights{ .cpu_read = true, .cpu_write = true, .dma = false, .grant = true };
    const mmio_ro_rights = kernel.Rights{ .cpu_read = true, .cpu_write = false, .dma = false, .grant = true };
    const cfg = device.config;
    installInitBlockMmioCapOrHalt(state, init_process_principal, label, "install common cap", cfg.common.page_paddr, mmio_rw_rights);
    installInitBlockMmioCapOrHalt(state, init_process_principal, label, "install notify cap", cfg.notify.page_paddr, mmio_rw_rights);
    if (cfg.isr.page_paddr != 0) installInitBlockMmioCapOrHalt(state, init_process_principal, label, "install isr cap", cfg.isr.page_paddr, mmio_ro_rights);
    if (cfg.device.page_paddr != 0) installInitBlockMmioCapOrHalt(state, init_process_principal, label, "install device cap", cfg.device.page_paddr, mmio_ro_rights);
    const init_submit_token = grantInitBlockQueueTokenOrHalt(state, init_process_principal, label, "queue submit grant", true, false);
    const init_notify_token = grantInitBlockQueueTokenOrHalt(state, init_process_principal, label, "queue notify grant", false, true);
    var updated = device;
    updated.descriptor.common_page_paddr = cfg.common.page_paddr;
    updated.descriptor.notify_page_paddr = cfg.notify.page_paddr;
    updated.descriptor.isr_page_paddr = cfg.isr.page_paddr;
    updated.descriptor.device_page_paddr = cfg.device.page_paddr;
    updated.descriptor.common_page_offset = cfg.common.page_offset;
    updated.descriptor.notify_page_offset = cfg.notify.page_offset;
    updated.descriptor.isr_page_offset = cfg.isr.page_offset;
    updated.descriptor.device_page_offset = cfg.device.page_offset;
    updated.descriptor.notify_off_multiplier = cfg.notify_off_multiplier;
    updated.descriptor.capacity_sectors = cfg.capacity_sectors;
    updated.descriptor.logical_block_size = cfg.logical_block_size;
    updated.descriptor.init_queue_submit_token = init_submit_token;
    updated.descriptor.init_queue_notify_token = init_notify_token;
    updated.descriptor.init_root_token = 0;
    return updated;
}

// ---------------------------------------------------------------------------
// Top-level setup entry point
// ---------------------------------------------------------------------------

pub fn setupInitBootstrapResources(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    boot_display_process: kernel.PrincipalId,
    input_devices: []?DetectedInputBootstrap,
    block_device: *?DetectedBlockBootstrap,
    bootfs_image: []const u8,
    framebuffer_info: ?uefi_services.FramebufferInfo,
    free_list: *kernel.FreePageList,
) void {
    const config_page = process_factory.allocAndMapOwnedPageForProcessOrHalt(
        state,
        init_process_principal,
        "init",
        "bootstrap config page",
        boot_abi.process_abi.standard_config_target_va,
        false,
        free_list,
    );
    const descriptor_page = process_factory.allocAndMapOwnedPageForProcessOrHalt(
        state,
        init_process_principal,
        "init",
        "bootstrap descriptor page",
        init_bootstrap_layout.descriptor_page_va,
        false,
        free_list,
    );
    const kernel_backed_pages = allocKernelBackedInitSpawnPages(state, init_process_principal, free_list);
    const init_services = buildInitServiceDescriptors(boot_display_process);
    mirrorKernelBackedInitSpawnPages(state, boot_display_process, kernel_backed_pages[0..]);
    publishInitServiceRegistryPage(kernel_backed_pages[0..], init_services.descriptors[0..init_services.count]);
    const registry_page = findKernelBackedInitSpawnPage(
        kernel_backed_pages[0..],
        init_bootstrap_layout.sourceVa(.window_service_config),
    ) orelse haltInitBootstrapDescriptor("missing window service config page");
    process_factory.installAndMapPageForProcessOrHalt(
        state,
        boot_display_process,
        registry_page.page,
        boot_abi.process_abi.service_registry_shadow_va,
        false,
        "boot_display",
        "shared service registry page",
    );
    const bootfs_setup = mapBootFsImageIntoProcessOrHalt(
        state,
        init_process_principal,
        "init",
        init_bootstrap_layout.bootfs_image_va,
        bootfs_image,
        free_list,
    );
    for (input_devices) |*entry| {
        const device = entry.* orelse continue;
        entry.* = setupInputDriverBootstrapForInit(state, init_process_principal, device, free_list);
    }
    if (block_device.*) |device| {
        block_device.* = setupBlockDriverBootstrapForInit(state, init_process_principal, device, free_list);
    }
    publishInitBootstrapConfigPage(config_page.paddr, init_bootstrap_layout.descriptor_page_va);
    publishInitBootstrapDescriptorPage(
        descriptor_page.paddr,
        input_devices,
        block_device.*,
        bootfs_setup,
        framebuffer_info,
    );
}

// ---------------------------------------------------------------------------
// Shell keyboard config page
// ---------------------------------------------------------------------------

pub fn publishShellKeyboardConfigPage(
    user_page_paddr: u64,
    cfg: ?MouseDriverConfig,
    queue_submit_token: u64,
    queue_notify_token: u64,
) void {
    const words: [*]volatile u64 = @ptrFromInt(user_page_paddr);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }
    if (cfg) |keyboard_cfg| {
        words[0] = 0x4B455942; // keyboard input config magic
        words[1] = keyboard_cfg.common.page_paddr;
        words[2] = keyboard_cfg.notify.page_paddr;
        words[3] = keyboard_cfg.isr.page_paddr;
        words[4] = keyboard_cfg.device.page_paddr;
        words[5] = keyboard_cfg.common.page_offset;
        words[6] = keyboard_cfg.notify.page_offset;
        words[7] = keyboard_cfg.isr.page_offset;
        words[8] = keyboard_cfg.device.page_offset;
        words[9] = keyboard_cfg.notify_off_multiplier;
        words[10] = 0;
        words[11] = 0;
        words[12] = 0;
        words[13] = queue_submit_token;
        words[14] = queue_notify_token;
        words[18] = 0;
    }
}
