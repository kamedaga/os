/// Init process bootstrap resource setup.
/// Sets up the init process's bootstrap descriptor page, spawn pages, service
/// registry, and virtio device capability grants.
const std = @import("std");
const capability = @import("../capability.zig");
const device_capabilities = @import("../device_capabilities.zig");
const kernel = @import("../kernel.zig");
const kernel_log = @import("../kernel_log.zig");
const boot_abi = @import("abi.zig");
const init_bootstrap_layout = @import("init_bootstrap_layout.zig");
const process_factory = @import("process_factory.zig");
const uefi_services = @import("uefi_services.zig");
const halt = @import("../halt.zig");

const init_bootstrap_abi = boot_abi.init_bootstrap_abi;
const service_registry_abi = boot_abi.service_registry_abi;
const boot_manifest_abi = boot_abi.boot_manifest_abi;

pub const MmioPageWithOffset = struct {
    page_paddr: u64,
    page_offset: u64,
};

pub const DetectedDeviceBootstrap = struct {
    descriptor: init_bootstrap_abi.DeviceDescriptor,
    dma_device: kernel.DmaDeviceId,
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

fn haltInitDeviceBootstrapError(label: []const u8, step: []const u8, err: anyerror) noreturn {
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

fn deviceLabel(device: kernel.DmaDeviceId) []const u8 {
    return switch (device) {
        .virtio_input => "virtio input device",
        .virtio_gpu => "virtio gpu device",
        .virtio_blk => "virtio block device",
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

fn buildInitServiceDescriptors() InitServiceDescriptorSet {
    // boot_display is now spawned by userland init, not pre-created by kernel.
    // The service registry will be populated by init once boot_display is running.
    return InitServiceDescriptorSet{};
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

pub fn refreshInitBootLogSnapshot(state: *kernel.KernelState, init_process_principal: kernel.PrincipalId) void {
    _ = state;
    const page_paddr = capability.lookupUserMappedPaddrForVa(init_process_principal, init_bootstrap_abi.boot_log_user_page_va) orelse
        haltInitBootstrapDescriptor("missing boot log snapshot page");
    const page: [*]u8 = @ptrFromInt(page_paddr);
    @memset(page[0..4096], 0);
    const copy_len: usize = @min(kernel_log.boot_log_len, init_bootstrap_abi.boot_log_page_payload_bytes);
    const length_ptr: *volatile u32 = @ptrFromInt(page_paddr + init_bootstrap_abi.boot_log_page_length_offset);
    const status_ptr: *volatile u32 = @ptrFromInt(page_paddr + init_bootstrap_abi.boot_log_page_status_offset);
    length_ptr.* = @intCast(copy_len);
    status_ptr.* = 1;
    if (copy_len != 0) {
        @memcpy(
            page[init_bootstrap_abi.boot_log_page_header_bytes .. init_bootstrap_abi.boot_log_page_header_bytes + copy_len],
            kernel_log.boot_log_buffer[0..copy_len],
        );
    }
}

pub fn publishInitBootstrapDescriptorPage(
    user_page_paddr: u64,
    devices: []const ?DetectedDeviceBootstrap,
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
    page.primary_display = .{ .flags = 0, .width = 0, .height = 0, .pitch = 0, .framebuffer_paddr = 0, .framebuffer_size_bytes = 0 };
    if (framebuffer_info) |info| {
        page.primary_display.flags = init_bootstrap_abi.display_flag_present;
        page.primary_display.width = info.width;
        page.primary_display.height = info.height;
        page.primary_display.pitch = info.pixels_per_scan_line;
        page.primary_display.framebuffer_paddr = info.paddr;
        page.primary_display.framebuffer_size_bytes = info.size_bytes;
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
            // boot_display is now spawned by userland init from bootfs, not kernel-loaded
            .boot_log_console => false,
            .vfs => false,
            .init_app => true,
            .bootfs_image => true,
        };
        if (present) {
            updated.flags |= boot_manifest_abi.image_flag_present | boot_manifest_abi.image_flag_kernel_loaded;
        }
        page.boot_images[idx] = updated;
    }

    var device_count: usize = 0;
    while (device_count < init_bootstrap_abi.max_device_descriptors) : (device_count += 1) {
        page.devices[device_count] = .{
            .transport = 0,
            .flags = 0,
            .bootstrap_source_va = 0,
            .vendor_id = 0,
            .device_id = 0,
            .subsystem_id = 0,
            .pci_bus = 0,
            .pci_device = 0,
            .pci_function = 0,
            .common_page_paddr = 0,
            .notify_page_paddr = 0,
            .isr_page_paddr = 0,
            .device_page_paddr = 0,
            .common_page_offset = 0,
            .notify_page_offset = 0,
            .isr_page_offset = 0,
            .device_page_offset = 0,
            .notify_off_multiplier = 0,
            .init_iommu_token = 0,
            .init_queue_grant_count = 0,
            .init_queue_grants = [_]init_bootstrap_abi.DeviceQueueGrant{.{}} ** init_bootstrap_abi.max_device_queue_grants,
            .init_command_token = 0,
        };
    }
    device_count = 0;
    for (devices) |entry| {
        const device = entry orelse continue;
        if (device_count >= init_bootstrap_abi.max_device_descriptors) break;
        var descriptor = device.descriptor;
        descriptor.flags |= init_bootstrap_abi.device_flag_present;
        page.devices[device_count] = descriptor;
        device_count += 1;
    }
    page.device_count = device_count;
}

// ---------------------------------------------------------------------------
// Generic device bootstrap for init
// ---------------------------------------------------------------------------

fn grantInitDeviceQueueTokenOrHalt(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    device: kernel.DmaDeviceId,
    label: []const u8,
    step_label: []const u8,
    queue_index: u16,
    submit: bool,
    notify: bool,
) u64 {
    return device_capabilities.queueCapGrantStage2(state, init_process_principal, device, queue_index, submit, notify) catch |err| {
        haltInitDeviceBootstrapError(label, step_label, err);
    };
}

fn bootstrapQueueGrantCountForDevice(device: kernel.DmaDeviceId) usize {
    return switch (device) {
        .virtio_gpu => 2,
        .virtio_input, .virtio_blk => 1,
    };
}

fn grantInitDeviceIommuTokenOrHalt(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    device: kernel.DmaDeviceId,
    label: []const u8,
    step_label: []const u8,
) u64 {
    return device_capabilities.iommuCapGrantStage2(state, init_process_principal, device, true, true, true) catch |err| {
        haltInitDeviceBootstrapError(label, step_label, err);
    };
}

fn blkCommandMask() u64 {
    return commandOpcodeBit(.blk_read) |
        commandOpcodeBit(.blk_write) |
        commandOpcodeBit(.blk_flush) |
        commandOpcodeBit(.blk_identify);
}

fn gpuCommandMask() u64 {
    return commandOpcodeBit(.gpu_admin) |
        commandOpcodeBit(.gpu_resource_2d) |
        commandOpcodeBit(.gpu_scanout) |
        commandOpcodeBit(.gpu_cursor) |
        commandOpcodeBit(.gpu_virgl_context) |
        commandOpcodeBit(.gpu_virgl_resource) |
        commandOpcodeBit(.gpu_virgl_submit) |
        commandOpcodeBit(.gpu_fence);
}

fn commandOpcodeBit(opcode: device_capabilities.CommandOpcodeClass) u64 {
    return @as(u64, 1) << @as(u6, @intCast(@intFromEnum(opcode)));
}

fn defaultCommandMaskForDevice(device: kernel.DmaDeviceId) u64 {
    return switch (device) {
        .virtio_blk => blkCommandMask(),
        .virtio_gpu => gpuCommandMask(),
        .virtio_input => commandOpcodeBit(.gpu_admin),
    };
}

fn grantInitDeviceCommandTokenOrHalt(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    device: kernel.DmaDeviceId,
    label: []const u8,
    step_label: []const u8,
) u64 {
    return device_capabilities.commandCapGrantStage2(
        state,
        init_process_principal,
        device,
        defaultCommandMaskForDevice(device),
    ) catch |err| {
        haltInitDeviceBootstrapError(label, step_label, err);
    };
}

pub fn setupDeviceBootstrapForInit(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    device: DetectedDeviceBootstrap,
    free_list: *kernel.FreePageList,
) DetectedDeviceBootstrap {
    const label = deviceLabel(device.dma_device);
    _ = process_factory.allocAndMapOwnedPageForProcessOrHalt(
        state,
        init_process_principal,
        "init",
        "device bootstrap page",
        device.descriptor.bootstrap_source_va,
        true,
        free_list,
    );
    var init_queue_grants = [_]init_bootstrap_abi.DeviceQueueGrant{.{}} ** init_bootstrap_abi.max_device_queue_grants;
    const queue_grant_count = bootstrapQueueGrantCountForDevice(device.dma_device);
    var queue_index: usize = 0;
    while (queue_index < queue_grant_count and queue_index < init_bootstrap_abi.max_device_queue_grants) : (queue_index += 1) {
        const queue_index_u16: u16 = @intCast(queue_index);
        const init_submit_token = grantInitDeviceQueueTokenOrHalt(
            state,
            init_process_principal,
            device.dma_device,
            label,
            "queue submit grant",
            queue_index_u16,
            true,
            false,
        );
        const init_notify_token = grantInitDeviceQueueTokenOrHalt(
            state,
            init_process_principal,
            device.dma_device,
            label,
            "queue notify grant",
            queue_index_u16,
            false,
            true,
        );
        init_queue_grants[queue_index] = .{
            .queue_index = @intCast(queue_index),
            .submit_token = init_submit_token,
            .notify_token = init_notify_token,
        };
    }
    const init_iommu_token = grantInitDeviceIommuTokenOrHalt(
        state,
        init_process_principal,
        device.dma_device,
        label,
        "iommu grant",
    );
    const init_command_token = grantInitDeviceCommandTokenOrHalt(
        state,
        init_process_principal,
        device.dma_device,
        label,
        "command grant",
    );
    var updated = device;
    updated.descriptor.init_iommu_token = init_iommu_token;
    updated.descriptor.init_queue_grant_count = @intCast(queue_grant_count);
    updated.descriptor.init_queue_grants = init_queue_grants;
    updated.descriptor.init_command_token = init_command_token;
    return updated;
}

// ---------------------------------------------------------------------------
// Top-level setup entry point
// ---------------------------------------------------------------------------

pub fn setupInitBootstrapResources(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    devices: []?DetectedDeviceBootstrap,
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
    _ = process_factory.allocAndMapOwnedPageForProcessOrHalt(
        state,
        init_process_principal,
        "init",
        "boot log snapshot page",
        init_bootstrap_abi.boot_log_user_page_va,
        false,
        free_list,
    );
    const kernel_backed_pages = allocKernelBackedInitSpawnPages(state, init_process_principal, free_list);
    const init_services = buildInitServiceDescriptors();
    publishInitServiceRegistryPage(kernel_backed_pages[0..], init_services.descriptors[0..init_services.count]);

    // Framebuffer and MMIO caps are now installed by init itself via install-mmio syscalls.
    // Kernel only provides physical addresses in the descriptor page.

    const bootfs_setup = mapBootFsImageIntoProcessOrHalt(
        state,
        init_process_principal,
        "init",
        init_bootstrap_layout.bootfs_image_va,
        bootfs_image,
        free_list,
    );
    for (devices) |*entry| {
        const device = entry.* orelse continue;
        entry.* = setupDeviceBootstrapForInit(state, init_process_principal, device, free_list);
    }
    publishInitBootstrapConfigPage(config_page.paddr, init_bootstrap_layout.descriptor_page_va);
    publishInitBootstrapDescriptorPage(
        descriptor_page.paddr,
        devices,
        bootfs_setup,
        framebuffer_info,
    );
    refreshInitBootLogSnapshot(state, init_process_principal);
}
