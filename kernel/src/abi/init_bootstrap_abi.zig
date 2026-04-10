const boot_manifest_abi = @import("boot_manifest_abi.zig");
const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x49425453; // "IBTS"
pub const version: u64 = 11;
pub const config_magic: u64 = 0x49425443; // "IBTC"
pub const config_version: u64 = 1;
pub const max_spawn_page_descriptors: usize = 8;
pub const max_input_device_descriptors: usize = 4;
pub const max_block_device_descriptors: usize = 2;
pub const max_boot_archive_pages: usize = 128;

pub const spawn_page_flag_kernel_backed: u64 = 1 << 0;
pub const spawn_page_flag_mirror_to_boot_display: u64 = 1 << 1;
pub const spawn_page_flag_mirror_writable: u64 = 1 << 2;
pub const spawn_page_flag_init_writable: u64 = 1 << 3;
pub const input_device_flag_present: u64 = 1 << 0;
pub const block_device_flag_present: u64 = 1 << 0;
pub const display_flag_present: u64 = 1 << 0;
pub const boot_archive_flag_present: u64 = 1 << 0;
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

pub const InputDeviceKind = enum(u64) {
    pointer = 1,
    keyboard = 2,
};

pub const BlockDeviceKind = enum(u64) {
    virtio_blk = 1,
};

pub const SpawnPageDescriptor = extern struct {
    kind: u64,
    subject: u64,
    flags: u64,
    source_va: u64,
    target_va: u64,
    spawn_flags: u64,
};

pub const InputDeviceDescriptor = extern struct {
    kind: u64,
    flags: u64,
    config_source_va: u64,
    config_target_va: u64,
    config_spawn_flags: u64,
    common_page_paddr: u64,
    notify_page_paddr: u64,
    isr_page_paddr: u64,
    device_page_paddr: u64,
    common_page_offset: u64,
    notify_page_offset: u64,
    isr_page_offset: u64,
    device_page_offset: u64,
    notify_off_multiplier: u64,
    init_queue_submit_token: u64,
    init_queue_notify_token: u64,
};

pub const BlockDeviceDescriptor = extern struct {
    kind: u64,
    flags: u64,
    config_source_va: u64,
    config_target_va: u64,
    config_spawn_flags: u64,
    common_page_paddr: u64,
    notify_page_paddr: u64,
    isr_page_paddr: u64,
    device_page_paddr: u64,
    common_page_offset: u64,
    notify_page_offset: u64,
    isr_page_offset: u64,
    device_page_offset: u64,
    notify_off_multiplier: u64,
    capacity_sectors: u64,
    logical_block_size: u64,
    init_queue_submit_token: u64,
    init_queue_notify_token: u64,
    init_root_token: u64,
};

pub const DisplayDescriptor = extern struct {
    flags: u64,
    width: u64,
    height: u64,
    pitch: u64,
};

pub const BootArchiveDescriptor = extern struct {
    flags: u64,
    image_va: u64,
    size_bytes: u64,
    page_count: u64,
};

pub const FileSystemDescriptor = extern struct {
    root_mount_token: u64,
    reserved0: u64 = 0,
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
    input_device_count: u64,
    block_device_count: u64,
    boot_image_count: u64,
    filesystem: FileSystemDescriptor,
    bootfs_archive: BootArchiveDescriptor,
    primary_display: DisplayDescriptor,
    spawn_pages: [max_spawn_page_descriptors]SpawnPageDescriptor,
    input_devices: [max_input_device_descriptors]InputDeviceDescriptor,
    block_devices: [max_block_device_descriptors]BlockDeviceDescriptor,
    boot_images: [boot_manifest_abi.max_boot_image_descriptors]boot_manifest_abi.BootImageDescriptor,
    bootfs_page_paddrs: [max_boot_archive_pages]u64,
};
