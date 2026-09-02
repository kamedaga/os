const table = @import("../state/table.zig");
const types = @import("../state/types.zig");

pub const AddressSpaceLockState = struct {
    value: u8 = 0,
    owner_cpu: usize = ~@as(usize, 0),
    depth: u32 = 0,
};

pub const UserAddressSpace = struct {
    // Includes user VA mappings plus supervisor-only helper PTs for return stacks.
    pub const max_dynamic_pdp_pages: usize = 64;
    pub const max_dynamic_pd_pages: usize = 64;
    pub const max_dynamic_pt_pages: usize = 512;
    pub const max_reservations: usize = 2048;
    pub const no_pd_index: u16 = 0xFFFF;

    pub const ReservationKind = enum(u8) {
        none = 0,
        bootstrap = 1,
        capability_page = 2,
        fresh_page = 3,
        linear_region = 4,
    };

    pub const Reservation = struct {
        base_va: u64 = 0,
        page_count: u32 = 0,
        generation: u32 = 0,
        kind: ReservationKind = .none,
        writable: bool = false,
        active: bool = false,
    };

    pml4: [512]u64 align(4096) = [_]u64{0} ** 512,
    pdp_pages: [max_dynamic_pdp_pages][512]u64 align(4096) = [_][512]u64{[_]u64{0} ** 512} ** max_dynamic_pdp_pages,
    pdp_page_pml4_index: [max_dynamic_pdp_pages]u16 = [_]u16{no_pd_index} ** max_dynamic_pdp_pages,
    pdp_page_used_len: u16 = 0,
    pd_pages: [max_dynamic_pd_pages][512]u64 align(4096) = [_][512]u64{[_]u64{0} ** 512} ** max_dynamic_pd_pages,
    pd_page_pml4_index: [max_dynamic_pd_pages]u16 = [_]u16{no_pd_index} ** max_dynamic_pd_pages,
    pd_page_pdp_index: [max_dynamic_pd_pages]u16 = [_]u16{no_pd_index} ** max_dynamic_pd_pages,
    pd_page_used_len: u16 = 0,
    pt_pages: [max_dynamic_pt_pages][512]u64 align(4096) = [_][512]u64{[_]u64{0} ** 512} ** max_dynamic_pt_pages,
    pt_page_pml4_index: [max_dynamic_pt_pages]u16 = [_]u16{no_pd_index} ** max_dynamic_pt_pages,
    pt_page_pdp_index: [max_dynamic_pt_pages]u16 = [_]u16{no_pd_index} ** max_dynamic_pt_pages,
    pt_page_pd_index: [max_dynamic_pt_pages]u16 = [_]u16{no_pd_index} ** max_dynamic_pt_pages,
    pt_page_used_len: u16 = 0,
    reservations: [max_reservations]Reservation = [_]Reservation{.{}} ** max_reservations,
    reservation_generation: u32 = 0,
    next_dynamic_map_page: u64 = 0,
    cr3: u64 = 0,
    // This object is address-stable: its page tables contain physical
    // pointers to the inline arrays above.  Keep the lock with that lifetime
    // and never reset or copy it as address-space contents are recycled.
    lock_state: AddressSpaceLockState = .{},
};

pub fn resetUserAddressSpaceStorage(space: *UserAddressSpace) void {
    @memset(space.pml4[0..], 0);
    for (0..UserAddressSpace.max_dynamic_pdp_pages) |index| {
        space.pdp_page_pml4_index[index] = UserAddressSpace.no_pd_index;
        @memset(space.pdp_pages[index][0..], 0);
    }
    for (0..UserAddressSpace.max_dynamic_pd_pages) |index| {
        space.pd_page_pml4_index[index] = UserAddressSpace.no_pd_index;
        space.pd_page_pdp_index[index] = UserAddressSpace.no_pd_index;
        @memset(space.pd_pages[index][0..], 0);
    }
    for (0..UserAddressSpace.max_dynamic_pt_pages) |index| {
        space.pt_page_pml4_index[index] = UserAddressSpace.no_pd_index;
        space.pt_page_pdp_index[index] = UserAddressSpace.no_pd_index;
        space.pt_page_pd_index[index] = UserAddressSpace.no_pd_index;
        @memset(space.pt_pages[index][0..], 0);
    }
    space.pdp_page_used_len = 0;
    space.pd_page_used_len = 0;
    space.pt_page_used_len = 0;
    for (space.reservations[0..]) |*reservation| reservation.* = .{};
    space.reservation_generation = 0;
    space.next_dynamic_map_page = 0;
    space.cr3 = 0;
}

fn allocKernelSlice(comptime T: type, free_list: *types.FreePageList, count: usize) ?[]T {
    if (count == 0) return null;
    const bytes = @sizeOf(T) * count;
    const page_count = (bytes + 4095) / 4096;
    const paddr = free_list.popContiguousAtOrAbove(page_count, 0) catch return null;
    const raw: [*]u8 = @ptrFromInt(paddr);
    @memset(raw[0 .. page_count * 4096], 0);
    const ptr: [*]T = @ptrCast(@alignCast(raw));
    return ptr[0..count];
}

pub const UserAddressSpaceTable = struct {
    inline_spaces: []UserAddressSpace = undefined,
    // Entries contain stable UserAddressSpace pointers encoded as usize.
    // The pointer directory itself follows the process table's copy-and-grow
    // rule; old directories remain valid for concurrent readers.
    extra_slots: [*]usize = undefined,
    capacity_value: usize = 0,

    pub fn init(self: *UserAddressSpaceTable, inline_spaces: []UserAddressSpace) void {
        self.inline_spaces = inline_spaces;
        self.extra_slots = undefined;
        @atomicStore(usize, &self.capacity_value, inline_spaces.len, .release);
    }

    pub fn capacity(self: *UserAddressSpaceTable) usize {
        return @atomicLoad(usize, &self.capacity_value, .acquire);
    }

    pub fn get(self: *UserAddressSpaceTable, index: usize) ?*UserAddressSpace {
        const current_capacity = self.capacity();
        if (index >= current_capacity) return null;
        if (index < self.inline_spaces.len) return &self.inline_spaces[index];
        const raw = @atomicLoad(usize, &self.extra_slots[index - self.inline_spaces.len], .acquire);
        if (raw == 0) return null;
        return @ptrFromInt(raw);
    }

    pub fn ensureCapacity(
        self: *UserAddressSpaceTable,
        required: usize,
        free_list: *types.FreePageList,
    ) bool {
        const old_capacity = self.capacity();
        if (required <= old_capacity) return true;
        const new_capacity = table.nextGeometricCapacity(
            old_capacity,
            self.inline_spaces.len,
            types.max_process_slots,
            required,
        ) orelse return false;
        const new_extra_count = new_capacity - self.inline_spaces.len;
        const old_extra_count = old_capacity - self.inline_spaces.len;
        const new_extra = allocKernelSlice(usize, free_list, new_extra_count) orelse return false;
        if (old_extra_count != 0) {
            @memcpy(new_extra[0..old_extra_count], self.extra_slots[0..old_extra_count]);
        }

        // Publish the fully initialized directory before its larger capacity.
        // UserAddressSpace bodies never move, so readers of either generation
        // retain valid pointers.
        self.extra_slots = new_extra.ptr;
        @atomicStore(usize, &self.capacity_value, new_capacity, .release);
        return true;
    }

    pub fn ensureSlot(
        self: *UserAddressSpaceTable,
        index: usize,
        free_list: *types.FreePageList,
    ) ?*UserAddressSpace {
        if (!self.ensureCapacity(index + 1, free_list)) return null;
        if (self.get(index)) |space| return space;
        if (index < self.inline_spaces.len) unreachable;

        const storage = allocKernelSlice(UserAddressSpace, free_list, 1) orelse return null;
        const space = &storage[0];
        space.lock_state = .{};
        resetUserAddressSpaceStorage(space);
        @atomicStore(
            usize,
            &self.extra_slots[index - self.inline_spaces.len],
            @intFromPtr(space),
            .release,
        );
        return space;
    }
};
