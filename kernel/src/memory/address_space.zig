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
