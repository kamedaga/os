//! PMM-to-mapped-storage adapter for the owned RAM mapping boundary.
//! No pointer-to-physical reconstruction: return the original physical extent.
const types = @import("../state/types.zig");
const layout = @import("../arch/x86_64/physical_layout.zig");
const profile = @import("../lock_profile.zig");
const std = @import("std");
const storage = @import("owned_storage.zig");

pub fn allocator(free_list: *types.FreePageList) storage.Allocator {
    return allocatorFor(free_list, .identity);
}
pub fn allocatorFor(free_list: *types.FreePageList, comptime caller: @import("../lock_profile.zig").PmmCaller) storage.Allocator {
    // New caller specializations must disappear completely from normal builds.
    if (comptime !profile.enabled and @intFromEnum(caller) >= 8) return allocator(free_list);
    return .{
        .context = free_list,
        .acquire = struct {
            fn acquire(context: *anyopaque, pages: usize) ?storage.MappedExtent {
                const fl: *types.FreePageList = @ptrCast(@alignCast(context));
                const sample = profile.storageBegin();
                var failed = true;
                defer profile.storageDetail(sample, caller, false, pages, failed);
                const grant = fl.popContiguousOwnedTagged(pages, 4096, layout.allocation_limit, caller) catch return null;
                // PMM guarantees a nonzero aligned extent below the direct-map
                // limit, so mapping cannot fail after physical ownership transfers.
                const bytes = layout.identitySlice(u8, grant.physical, pages * 4096) orelse unreachable;
                failed = false;
                return .{ .allocation = grant, .bytes = @alignCast(bytes) };
            }
        }.acquire,
        .release = if (comptime profile.enabled) struct {
            fn taggedRelease(context: *anyopaque, grant: storage.PhysicalExtent) bool {
                const sample = profile.storageBegin();
                const success = release(context, grant);
                profile.storageDetail(sample, caller, true, grant.pages, !success);
                return success;
            }
        }.taggedRelease else release,
    };
}

fn release(context: *anyopaque, grant: storage.PhysicalExtent) bool {
    const free_list: *types.FreePageList = @ptrCast(@alignCast(context));
    // Range-metadata exhaustion is recoverable: the owner retains the entire
    // unpublished allocation for retry; PMM derives its region from physical RAM.
    free_list.appendContiguousRange(grant.physical.toInt(), grant.pages) catch |err| switch (err) {
        error.PageQuarantined => return false,
        else => @panic("invalid PMM return"),
    };
    return true;
}

/// Only the generic PMM adapter is retagged. Custom allocators and already
/// explicit owner tags retain their callbacks and all failure semantics.
pub fn forType(original: storage.Allocator, comptime T: type) storage.Allocator {
    if (comptime !profile.enabled) return original;
    const generic = allocator(undefined);
    if (original.acquire != generic.acquire or original.release != generic.release) return original;
    const fl: *types.FreePageList = @ptrCast(@alignCast(original.context));
    const name = @typeName(T);
    const caller: profile.PmmCaller = comptime blk: {
        @setEvalBranchQuota(100000);
        if (std.mem.indexOf(u8, name, "reference_index") != null or std.mem.indexOf(u8, name, "ObjectReferences") != null) break :blk .reference_index;
        if (std.mem.indexOf(u8, name, "slot_table") != null) {
            if (std.mem.endsWith(u8, name, ".Node")) break :blk .slot_directory;
            if (std.mem.endsWith(u8, name, ".Block")) break :blk .slot_metadata;
        }
        if (std.mem.indexOf(u8, name, "Fd") != null) break :blk .fd_table_storage;
        if (std.mem.indexOf(u8, name, "Thread") != null or std.mem.indexOf(u8, name, "scheduler_runqueue") != null) break :blk .thread_storage;
        if (std.mem.indexOf(u8, name, "Process") != null) break :blk .process_storage;
        if (std.mem.indexOf(u8, name, "UserAddressSpace") != null) break :blk .address_space_storage;
        if (std.mem.indexOf(u8, name, "Vma") != null) break :blk .vma_storage;
        if (std.mem.indexOf(u8, name, "slot_table") != null and std.mem.endsWith(u8, name, ".Body")) break :blk .slot_body;
        break :blk .identity;
    };
    return allocatorFor(fl, caller);
}
