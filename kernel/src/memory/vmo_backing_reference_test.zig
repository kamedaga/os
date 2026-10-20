//! Host-only old open-addressing implementation, mechanically retained from
//! the pre-migration types.zig. No production import or duplicate live store.
const std = @import("std");
comptime {
    if (!@import("builtin").is_test) @compileError("host reference only");
}
const Error = error{ InvalidState, TableFull };
pub const Hash = struct {
    paddrs: [4096]u64 = .{0} ** 4096,
    owners: [4096]u64 = .{0} ** 4096,
    indices: [4096]u32 = .{0} ** 4096,
    used: usize = 0,
    fn mixVmoBackingPageStoreKey(owner: u64, page_index: u32) u64 {
        var value = owner ^ (@as(u64, page_index) *% 0x9e37_79b9_7f4a_7c15);
        value ^= value >> 30;
        value *%= 0xbf58_476d_1ce4_e5b9;
        value ^= value >> 27;
        value *%= 0x94d0_49bb_1331_11eb;
        value ^= value >> 31;
        return value;
    }

    pub fn vmoBackingPageStoreBucketLocked(self: *@This(), owner: u64, page_index: u32) usize {
        return @intCast(mixVmoBackingPageStoreKey(owner, page_index) % self.paddrs.len);
    }

    pub fn findVmoBackingPageStoreSlotLocked(self: *@This(), owner: u64, page_index: u32) ?usize {
        if (!true or owner == 0) return null;
        var slot_index = self.vmoBackingPageStoreBucketLocked(owner, page_index);
        var probes: usize = 0;
        while (probes < self.paddrs.len) : (probes += 1) {
            const actual_owner = self.owners[slot_index];
            if (actual_owner == 0) return null;
            if (actual_owner == owner and
                self.indices[slot_index] == page_index)
            {
                return slot_index;
            }
            slot_index = if (slot_index + 1 == self.paddrs.len) 0 else slot_index + 1;
        }
        return null;
    }

    pub fn insertVmoBackingPageStoreSlotLocked(self: *@This(), owner: u64, page_index: u32, paddr: u64) Error!usize {
        if (!true or owner == 0 or paddr == 0 or
            (paddr & 0xFFF) != 0)
        {
            return Error.InvalidState;
        }
        var slot_index = self.vmoBackingPageStoreBucketLocked(owner, page_index);
        var probes: usize = 0;
        while (probes < self.paddrs.len) : (probes += 1) {
            const actual_owner = self.owners[slot_index];
            if (actual_owner == 0) {
                // Keep one physical slot empty so delete/reinsert cannot turn the
                // table into a probe cycle with no terminating empty bucket.
                if (self.used >= self.paddrs.len - 1) {
                    return Error.TableFull;
                }
                self.paddrs[slot_index] = paddr;
                self.indices[slot_index] = page_index;
                self.owners[slot_index] = owner;
                self.used += 1;
                return slot_index;
            }
            if (actual_owner == owner and
                self.indices[slot_index] == page_index)
            {
                self.paddrs[slot_index] = paddr;
                return slot_index;
            }
            slot_index = if (slot_index + 1 == self.paddrs.len) 0 else slot_index + 1;
        }
        return Error.TableFull;
    }

    pub fn removeVmoBackingPageStoreSlotLocked(self: *@This(), slot_index: usize) void {
        if (slot_index >= self.paddrs.len or
            self.owners[slot_index] == 0)
        {
            return;
        }
        self.paddrs[slot_index] = 0;
        self.indices[slot_index] = 0;
        self.owners[slot_index] = 0;
        self.used -= 1;

        var scan = if (slot_index + 1 == self.paddrs.len) 0 else slot_index + 1;
        var probes: usize = 0;
        while (probes < self.paddrs.len) : (probes += 1) {
            const owner = self.owners[scan];
            if (owner == 0) return;
            const page_index = self.indices[scan];
            const paddr = self.paddrs[scan];
            self.paddrs[scan] = 0;
            self.indices[scan] = 0;
            self.owners[scan] = 0;
            self.used -= 1;
            _ = self.insertVmoBackingPageStoreSlotLocked(owner, page_index, paddr) catch unreachable;
            scan = if (scan + 1 == self.paddrs.len) 0 else scan + 1;
        }
        unreachable;
    }
};
