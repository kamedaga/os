pub const max_boot_image_descriptors: usize = 4;

pub const image_flag_present: u64 = 1 << 0;
pub const image_flag_kernel_loaded: u64 = 1 << 1;

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
