const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x53525643; // "SRVC"
pub const version: u64 = 1;
pub const page_va: u64 = process_abi.auxPageVa(5);
pub const max_entries: usize = 6;
pub const dynamic_endpoint_id_base: u64 = 0x80;

pub const ServiceKind = enum(u64) {
    window = 1,
    vfs = 2,
    block = 4,
    persistent_fs = 5,
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

pub fn writeSingleService(base_va: u64, kind: ServiceKind, process_slot: u64, endpoint_id: u64) void {
    initPage(base_va);
    addService(base_va, kind, process_slot, endpoint_id);
}

pub fn addService(base_va: u64, kind: ServiceKind, process_slot: u64, endpoint_id: u64) void {
    const page: *volatile RegistryPage = @ptrFromInt(base_va);
    if (page.magic != magic or page.version != version) initPage(base_va);
    if (page.entry_count >= max_entries) return;
    const index: usize = @intCast(page.entry_count);
    page.entries[index] = .{
        .kind = @intFromEnum(kind),
        .process_slot = process_slot,
        .endpoint_id = endpoint_id,
        .flags = 0,
    };
    page.entry_count += 1;
}

pub fn findService(base_va: u64, kind: ServiceKind) ?ServiceEntry {
    const page: *volatile RegistryPage = @ptrFromInt(base_va);
    if (page.magic != magic or page.version != version) return null;
    var i: usize = 0;
    while (i < page.entry_count and i < max_entries) : (i += 1) {
        const entry = page.entries[i];
        if (entry.kind == @intFromEnum(kind)) return entry;
    }
    return null;
}
