pub const syscall_register_iommu_driver: u64 = 0x27;

pub const DeviceId = enum(u8) {
    virtio_gpu = 0,
    virtio_input = 1,
};
