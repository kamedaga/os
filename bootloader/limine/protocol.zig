pub const common_magic0: u64 = 0xc7b1dd30df4c8b88;
pub const common_magic1: u64 = 0x0a82e883a194f07b;

pub fn requestId(comptime a: u64, comptime b: u64) [4]u64 {
    return .{ common_magic0, common_magic1, a, b };
}

pub const base_revision_magic0: u64 = 0xf9562b2d5c95a6c8;
pub const base_revision_magic1: u64 = 0x6a7b384944536bdc;
pub const requests_start_marker: [4]u64 = .{
    0xf6b8f4b39de7d1ae,
    0xfab91a6940fcb9cf,
    0x785c6ed015d3e316,
    0x181e920a7852b9d9,
};
pub const requests_end_marker: [2]u64 = .{
    0xadc0e0531bb10d03,
    0x9572709f31764c62,
};

pub const hhdm_request_id = requestId(0x48dcf1cb8ad2b852, 0x63984e959a98244b);
pub const framebuffer_request_id = requestId(0x9d5827dcd881dd75, 0xa3148604f6fab11b);
pub const memmap_request_id = requestId(0x67cf3d9d378a806f, 0xe304acdfc50c3c62);
pub const module_request_id = requestId(0x3e7e279702be32af, 0xca1c4f3bd1280cee);
pub const rsdp_request_id = requestId(0xc5e77b6b397e7b43, 0x27637845accdcf3c);
pub const executable_address_request_id = requestId(0x71ba76863cc55f63, 0xb2644a48c516a487);

pub const RequestHeader = extern struct {
    id: [4]u64,
    revision: u64,
    response: ?*anyopaque,
};

pub const HhdmRequest = extern struct {
    id: [4]u64 = hhdm_request_id,
    revision: u64 = 0,
    response: ?*HhdmResponse = null,
};

pub const HhdmResponse = extern struct {
    revision: u64,
    offset: u64,
};

pub const FramebufferRequest = extern struct {
    id: [4]u64 = framebuffer_request_id,
    revision: u64 = 0,
    response: ?*FramebufferResponse = null,
};

pub const FramebufferResponse = extern struct {
    revision: u64,
    framebuffer_count: u64,
    framebuffers: [*]?*Framebuffer,
};

pub const framebuffer_rgb: u8 = 1;

pub const Framebuffer = extern struct {
    address: ?*anyopaque,
    width: u64,
    height: u64,
    pitch: u64,
    bpp: u16,
    memory_model: u8,
    red_mask_size: u8,
    red_mask_shift: u8,
    green_mask_size: u8,
    green_mask_shift: u8,
    blue_mask_size: u8,
    blue_mask_shift: u8,
    unused: [7]u8,
    edid_size: u64,
    edid: ?*anyopaque,
    mode_count: u64,
    modes: [*]?*VideoMode,
};

pub const VideoMode = extern struct {
    pitch: u64,
    width: u64,
    height: u64,
    bpp: u16,
    memory_model: u8,
    red_mask_size: u8,
    red_mask_shift: u8,
    green_mask_size: u8,
    green_mask_shift: u8,
    blue_mask_size: u8,
    blue_mask_shift: u8,
};

pub const MemmapRequest = extern struct {
    id: [4]u64 = memmap_request_id,
    revision: u64 = 0,
    response: ?*MemmapResponse = null,
};

pub const MemmapResponse = extern struct {
    revision: u64,
    entry_count: u64,
    entries: [*]?*MemmapEntry,
};

pub const MemmapEntry = extern struct {
    base: u64,
    length: u64,
    kind: MemmapEntryKind,
};

pub const MemmapEntryKind = enum(u64) {
    usable = 0,
    reserved = 1,
    acpi_reclaimable = 2,
    acpi_nvs = 3,
    bad_memory = 4,
    bootloader_reclaimable = 5,
    executable_and_modules = 6,
    framebuffer = 7,
};

pub const internal_module_required: u64 = 1 << 0;

pub const InternalModule = extern struct {
    path: [*:0]const u8,
    string: [*:0]const u8,
    flags: u64,
};

pub const ModuleRequest = extern struct {
    id: [4]u64 = module_request_id,
    revision: u64 = 1,
    response: ?*ModuleResponse = null,
    internal_module_count: u64,
    internal_modules: ?[*]?*InternalModule,
};

pub const ModuleResponse = extern struct {
    revision: u64,
    module_count: u64,
    modules: [*]?*File,
};

pub const File = extern struct {
    revision: u64,
    address: ?*anyopaque,
    size: u64,
    path: [*:0]u8,
    string: [*:0]u8,
    media_type: u32,
    unused: u32,
    tftp_ip: u32,
    tftp_port: u32,
    partition_index: u32,
    mbr_disk_id: u32,
    gpt_disk_uuid: Uuid,
    gpt_part_uuid: Uuid,
    part_uuid: Uuid,
};

pub const Uuid = extern struct {
    a: u32,
    b: u16,
    c: u16,
    d: [8]u8,
};

pub const RsdpRequest = extern struct {
    id: [4]u64 = rsdp_request_id,
    revision: u64 = 0,
    response: ?*RsdpResponse = null,
};

pub const RsdpResponse = extern struct {
    revision: u64,
    address: u64,
};

pub const ExecutableAddressRequest = extern struct {
    id: [4]u64 = executable_address_request_id,
    revision: u64 = 0,
    response: ?*ExecutableAddressResponse = null,
};

pub const ExecutableAddressResponse = extern struct {
    revision: u64,
    physical_base: u64,
    virtual_base: u64,
};
