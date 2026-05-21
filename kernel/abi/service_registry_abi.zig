const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x53525643; // "SRVC"
pub const version: u64 = 1;
pub const page_va: u64 = process_abi.auxPageVa(5);
pub const max_entries: usize = 8;
pub const dynamic_endpoint_id_base: u64 = 0x80;
pub const syscall_publish_service_endpoint: u64 = 0x33;
pub const service_flag_process_slot_compat: u64 = 1 << 0;

pub const ServiceKind = enum(u64) {
    window = 1,
    vfs = 2,
    block = 4,
    persistent_fs = 5,
    capctl = 6,
    gpu = 7,
    pointer = 8,
};

pub const ServiceEntry = extern struct {
    kind: u64,
    process_slot: u64,
    endpoint_id: u64,
    flags: u64,
};

pub const RegistryPage = extern struct {
    magic: u64,
    version: u64,
    entry_count: u64,
    reserved0: u64,
    entries: [max_entries]ServiceEntry,
};

pub fn clearPage(base_va: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }
}

pub fn initPage(base_va: u64) void {
    clearPage(base_va);
    const page: *volatile RegistryPage = @ptrFromInt(base_va);
    page.magic = magic;
    page.version = version;
    page.entry_count = 0;
}

pub fn addService(base_va: u64, kind: ServiceKind, endpoint_id: u64) void {
    const page: *volatile RegistryPage = @ptrFromInt(base_va);
    if (page.magic != magic or page.version != version) initPage(base_va);
    if (page.entry_count >= max_entries) return;
    const index: usize = @intCast(page.entry_count);
    const slot = &page.entries[index];
    slot.kind = @intFromEnum(kind);
    slot.process_slot = 0;
    slot.endpoint_id = endpoint_id;
    slot.flags = service_flag_process_slot_compat;
    page.entry_count += 1;
}
