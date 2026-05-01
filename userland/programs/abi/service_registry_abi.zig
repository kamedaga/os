const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x53525643; // "SRVC"
pub const version: u64 = 1;
pub const page_va: u64 = process_abi.auxPageVa(5);
pub const max_entries: usize = 12;
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
    fat_fs = 9,
};

pub const ServiceEntry = extern struct {
    kind: u64,
    // Optional compatibility field. Public service lookup should use endpoint_id.
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

pub fn writeSingleService(base_va: u64, kind: ServiceKind, endpoint_id: u64) void {
    initPage(base_va);
    addService(base_va, kind, endpoint_id);
}

pub fn writeSingleServiceWithProcessSlot(base_va: u64, kind: ServiceKind, process_slot: u64, endpoint_id: u64) void {
    initPage(base_va);
    addServiceWithProcessSlot(base_va, kind, process_slot, endpoint_id);
}

pub fn addService(base_va: u64, kind: ServiceKind, endpoint_id: u64) void {
    addServiceWithProcessSlot(base_va, kind, 0, endpoint_id);
}

pub fn addServiceWithProcessSlot(base_va: u64, kind: ServiceKind, process_slot: u64, endpoint_id: u64) void {
    addServiceEntry(base_va, .{
        .kind = @intFromEnum(kind),
        .process_slot = process_slot,
        .endpoint_id = endpoint_id,
        .flags = service_flag_process_slot_compat,
    });
}

pub fn setServiceWithProcessSlot(base_va: u64, kind: ServiceKind, process_slot: u64, endpoint_id: u64) void {
    const page: *volatile RegistryPage = @ptrFromInt(base_va);
    if (page.magic != magic or page.version != version) initPage(base_va);
    var i: usize = 0;
    while (i < page.entry_count and i < max_entries) : (i += 1) {
        const entry = &page.entries[i];
        if (entry.kind != @intFromEnum(kind)) continue;
        entry.process_slot = process_slot;
        entry.endpoint_id = endpoint_id;
        entry.flags = service_flag_process_slot_compat;
        return;
    }
    addServiceWithProcessSlot(base_va, kind, process_slot, endpoint_id);
}

pub fn removeService(base_va: u64, kind: ServiceKind) void {
    const page: *volatile RegistryPage = @ptrFromInt(base_va);
    if (page.magic != magic or page.version != version) return;
    var i: usize = 0;
    while (i < page.entry_count and i < max_entries) : (i += 1) {
        if (page.entries[i].kind != @intFromEnum(kind)) continue;
        var j: usize = i;
        while (j + 1 < page.entry_count and j + 1 < max_entries) : (j += 1) {
            page.entries[j] = page.entries[j + 1];
        }
        if (page.entry_count != 0) {
            page.entry_count -= 1;
            page.entries[page.entry_count] = .{ .kind = 0, .process_slot = 0, .endpoint_id = 0, .flags = 0 };
        }
        return;
    }
}

pub fn addServiceEntry(base_va: u64, entry: ServiceEntry) void {
    const page: *volatile RegistryPage = @ptrFromInt(base_va);
    if (page.magic != magic or page.version != version) initPage(base_va);
    if (page.entry_count >= max_entries) return;
    const index: usize = @intCast(page.entry_count);
    const slot = &page.entries[index];
    slot.kind = entry.kind;
    slot.process_slot = entry.process_slot;
    slot.endpoint_id = entry.endpoint_id;
    slot.flags = entry.flags;
    page.entry_count += 1;
}

pub fn findService(base_va: u64, kind: ServiceKind) ?ServiceEntry {
    const page: *volatile RegistryPage = @ptrFromInt(base_va);
    if (page.magic != magic or page.version != version) return null;
    var i: usize = 0;
    while (i < page.entry_count and i < max_entries) : (i += 1) {
        const entry = &page.entries[i];
        if (entry.kind == @intFromEnum(kind)) {
            return .{
                .kind = entry.kind,
                .process_slot = entry.process_slot,
                .endpoint_id = entry.endpoint_id,
                .flags = entry.flags,
            };
        }
    }
    return null;
}

pub fn allowsProcessSlotCompat(entry: ServiceEntry) bool {
    return entry.process_slot != 0 and (entry.flags & service_flag_process_slot_compat) != 0;
}
