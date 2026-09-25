//! The diagnostic executables use the same Limine request set as the live
//! kernel. Keeping the internal modules required makes a successful handoff
//! comparable to the failing real-machine boot, without running userland.
const limine = @import("protocol.zig");

pub export var limine_requests_start align(8) linksection(".limine_requests_start") = limine.requests_start_marker;

pub export var limine_base_revision align(8) linksection(".limine_requests") = [3]u64{
    limine.base_revision_magic0,
    limine.base_revision_magic1,
    3,
};

const init_module_path: [*:0]const u8 = "INITAPP.ELF";
const init_module_tag: [*:0]const u8 = "init";
const bootfs_module_path: [*:0]const u8 = "BOOTFS.IMG";
const bootfs_module_tag: [*:0]const u8 = "bootfs";

pub export var init_internal_module align(8) linksection(".limine_requests") = limine.InternalModule{
    .path = init_module_path,
    .string = init_module_tag,
    .flags = limine.internal_module_required,
};
pub export var bootfs_internal_module align(8) linksection(".limine_requests") = limine.InternalModule{
    .path = bootfs_module_path,
    .string = bootfs_module_tag,
    .flags = limine.internal_module_required,
};
pub export var internal_modules align(8) linksection(".limine_requests") = [2]?*limine.InternalModule{
    &init_internal_module,
    &bootfs_internal_module,
};

pub export var hhdm_request align(8) linksection(".limine_requests") = limine.HhdmRequest{};
pub export var framebuffer_request align(8) linksection(".limine_requests") = limine.FramebufferRequest{};
pub export var memmap_request align(8) linksection(".limine_requests") = limine.MemmapRequest{};
pub export var module_request align(8) linksection(".limine_requests") = limine.ModuleRequest{
    .internal_module_count = internal_modules.len,
    .internal_modules = &internal_modules,
};
pub export var rsdp_request align(8) linksection(".limine_requests") = limine.RsdpRequest{};
pub export var executable_address_request align(8) linksection(".limine_requests") = limine.ExecutableAddressRequest{};

pub export var limine_requests_end align(8) linksection(".limine_requests_end") = limine.requests_end_marker;
