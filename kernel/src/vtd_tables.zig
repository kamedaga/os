pub const entries_per_page: usize = 512;
pub const TablePage = [entries_per_page]u64;

pub const page_size: u64 = 4096;
pub const page_address_mask: u64 = 0x000f_ffff_ffff_f000;
pub const root_present: u64 = 1 << 0;
pub const context_present: u64 = 1 << 0;
pub const context_address_width_4_level: u64 = 2;
pub const second_level_read: u64 = 1 << 0;
pub const second_level_write: u64 = 1 << 1;

fn validPageAddress(paddr: u64) bool {
    return (paddr & (page_size - 1)) == 0 and (paddr & ~page_address_mask) == 0;
}

pub fn clear(page: *TablePage) void {
    @memset(page[0..], 0);
}

/// Install one 128-bit legacy root entry. Each PCI bus selects one root
/// entry and therefore one 4 KiB context table.
pub fn setRootEntry(page: *TablePage, bus: u8, context_table_paddr: u64) bool {
    if (!validPageAddress(context_table_paddr)) return false;
    const word_index = @as(usize, bus) * 2;
    page[word_index] = context_table_paddr | root_present;
    page[word_index + 1] = 0;
    return true;
}

/// Install one 128-bit legacy context entry using second-level translation,
/// a four-level (48-bit) adjusted guest-address width, and the supplied DID.
pub fn setContextEntry(
    page: *TablePage,
    device_function: u8,
    second_level_root_paddr: u64,
    domain_id: u16,
) bool {
    if (!validPageAddress(second_level_root_paddr)) return false;
    const word_index = @as(usize, device_function) * 2;
    // TT is zero for legacy second-level translation.
    page[word_index] = second_level_root_paddr | context_present;
    page[word_index + 1] =
        (@as(u64, domain_id) << 8) | context_address_width_4_level;
    return true;
}

/// Install one second-level paging entry. This builder is used for both
/// non-leaf links and 4 KiB leaves; callers choose the physical target.
pub fn setSecondLevelEntry(
    page: *TablePage,
    index: usize,
    paddr: u64,
    readable: bool,
    writable: bool,
) bool {
    if (index >= page.len or !validPageAddress(paddr)) return false;
    page[index] = paddr |
        (if (readable) second_level_read else 0) |
        (if (writable) second_level_write else 0);
    return true;
}
