const process_abi = @import("process_abi.zig");

pub const max_boot_image_descriptors: usize = 4;
pub const image_flag_present: u64 = 1 << 0;
pub const image_flag_kernel_loaded: u64 = 1 << 1;

pub const magic: u64 = 0x49425453; // "IBTS"
pub const version: u64 = 18;
pub const config_magic: u64 = 0x49425443; // "IBTC"
pub const config_version: u64 = 1;
pub const max_spawn_page_descriptors: usize = 8;
pub const max_device_descriptors: usize = 8;
pub const max_device_queue_grants: usize = 4;
pub const max_boot_archive_pages: usize = 128;
pub const boot_display_shell_height: u64 = 624;
pub const boot_log_user_page_va: u64 = process_abi.auxPageVa(1);
pub const boot_log_page_header_bytes: usize = 8;
pub const boot_log_page_payload_bytes: usize = 4096 - boot_log_page_header_bytes;
pub const boot_log_page_length_offset: usize = 0;
pub const boot_log_page_status_offset: usize = 4;

/// Indices (in u64 words) within boot_display's config page for framebuffer metadata.
pub const boot_display_config_width_index: u64 = 17;
pub const boot_display_config_height_index: u64 = 18;
pub const boot_display_config_pitch_index: u64 = 19;
pub const boot_display_config_fb_paddr_index: u64 = 20;
pub const boot_display_config_fb_size_bytes_index: u64 = 21;
pub const boot_display_config_fb_vm_token_index: u64 = 22;
pub const boot_display_config_pointer_shared_va_index: u64 = 23;

pub const spawn_page_flag_kernel_backed: u64 = 1 << 0;
pub const spawn_page_flag_mirror_to_boot_display: u64 = 1 << 1;
pub const spawn_page_flag_mirror_writable: u64 = 1 << 2;
pub const spawn_page_flag_init_writable: u64 = 1 << 3;
pub const device_flag_present: u64 = 1 << 0;
pub const display_flag_present: u64 = 1 << 0;
pub const boot_archive_flag_present: u64 = 1 << 0;
pub const ImageKind = enum(u64) {
    boot_log_console = 1,
    vfs = 2,
    init_app = 3,
    bootfs_image = 4,
};

pub const ImagePayloadKind = enum(u64) {
    elf = 1,
    archive = 2,
};

pub const BootImageDescriptor = extern struct {
    kind: u64,
    payload_kind: u64,
    flags: u64,
};

pub const builtin_boot_images = [_]BootImageDescriptor{
    .{
        .kind = @intFromEnum(ImageKind.boot_log_console),
        .payload_kind = @intFromEnum(ImagePayloadKind.elf),
        .flags = 0,
    },
    .{
        .kind = @intFromEnum(ImageKind.vfs),
        .payload_kind = @intFromEnum(ImagePayloadKind.elf),
        .flags = 0,
    },
    .{
        .kind = @intFromEnum(ImageKind.init_app),
        .payload_kind = @intFromEnum(ImagePayloadKind.elf),
        .flags = 0,
    },
    .{
        .kind = @intFromEnum(ImageKind.bootfs_image),
        .payload_kind = @intFromEnum(ImagePayloadKind.archive),
        .flags = 0,
    },
};

pub const SpawnPageKind = enum(u64) {
    ui_config = 1,
    ui_state = 2,
    ui_command = 3,
    input_shared = 4,
    service_config = 5,
};

pub const SpawnPageSubject = enum(u64) {
    primary_panel = 1,
    pointer = 2,
    keyboard = 3,
    window_service = 4,
};

pub const DeviceTransport = enum(u64) {
    virtio_pci_modern = 1,
    pci_function = 2,
};

pub const SpawnPageDescriptor = extern struct {
    kind: u64,
    subject: u64,
    flags: u64,
    source_va: u64,
    target_va: u64,
    spawn_flags: u64,
};

pub const DeviceQueueGrant = extern struct {
    queue_index: u64 = 0,
    submit_token: u64 = 0,
    notify_token: u64 = 0,
};

pub const DeviceDescriptor = extern struct {
    transport: u64,
    flags: u64,
    bootstrap_source_va: u64,
    vendor_id: u64,
    device_id: u64,
    subsystem_id: u64,
    pci_bus: u64,
    pci_device: u64,
    pci_function: u64,
    resource_id: u64,
    queue_count: u64,
    common_page_paddr: u64,
    notify_page_paddr: u64,
    isr_page_paddr: u64,
    device_page_paddr: u64,
    common_page_offset: u64,
    notify_page_offset: u64,
    isr_page_offset: u64,
    device_page_offset: u64,
    notify_off_multiplier: u64,
    init_iommu_token: u64,
    init_queue_grant_count: u64,
    init_queue_grants: [max_device_queue_grants]DeviceQueueGrant,
    init_command_token: u64,
    init_device_fd: u64,
};

pub const DisplayDescriptor = extern struct {
    flags: u64,
    width: u64,
    height: u64,
    pitch: u64,
    /// Physical address of the framebuffer MMIO region.
    /// Init grants the MMIO pages directly to boot_display.
    framebuffer_paddr: u64 = 0,
    /// Exact byte size of the framebuffer region.
    framebuffer_size_bytes: u64 = 0,
};

pub const BootArchiveDescriptor = extern struct {
    flags: u64,
    image_va: u64,
    size_bytes: u64,
    page_count: u64,
};

pub const ConfigPage = extern struct {
    magic: u64,
    version: u64,
    descriptor_page_va: u64,
    reserved0: u64 = 0,
};

pub const DescriptorPage = extern struct {
    magic: u64,
    version: u64,
    spawn_page_count: u64,
    device_count: u64,
    boot_image_count: u64,
    _reserved_counts: [3]u64 = .{ 0, 0, 0 },
    bootfs_archive: BootArchiveDescriptor,
    primary_display: DisplayDescriptor,
    spawn_pages: [max_spawn_page_descriptors]SpawnPageDescriptor,
    devices: [max_device_descriptors]DeviceDescriptor,
    boot_images: [max_boot_image_descriptors]BootImageDescriptor,
    bootfs_page_paddrs: [max_boot_archive_pages]u64,
};
