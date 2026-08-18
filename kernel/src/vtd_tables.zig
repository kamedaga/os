pub const entries_per_page: usize = 512;
pub const TablePage = [entries_per_page]u64;

pub const page_size: u64 = 4096;
pub const page_address_mask: u64 = 0x000f_ffff_ffff_f000;
pub const root_present: u64 = 1 << 0;
pub const context_present: u64 = 1 << 0;
pub const context_address_width_4_level: u64 = 2;
pub const second_level_read: u64 = 1 << 0;
pub const second_level_write: u64 = 1 << 1;
pub const iova_window_start: u64 = 0x8000_0000;
pub const iova_window_end: u64 = 0x9000_0000;
pub const iova_page_count: usize = @intCast((iova_window_end - iova_window_start) / page_size);
pub const iova_bitmap_bytes: usize = iova_page_count / 8;

pub const IovaAllocator = struct {
    bitmap: ?*[iova_bitmap_bytes]u8 = null,
    cursor: usize = 0,
    used_pages: usize = 0,
    peak_pages: usize = 0,

    pub fn init(bitmap: *[iova_bitmap_bytes]u8) IovaAllocator {
        @memset(bitmap[0..], 0);
        return .{ .bitmap = bitmap };
    }

    fn bitIsSet(self: *const IovaAllocator, page_index: usize) bool {
        const bitmap = self.bitmap orelse return false;
        const byte = bitmap[page_index / 8];
        return (byte & (@as(u8, 1) << @intCast(page_index & 7))) != 0;
    }

    fn setBit(self: *IovaAllocator, page_index: usize, allocated: bool) void {
        const bitmap = self.bitmap orelse return;
        const mask = @as(u8, 1) << @intCast(page_index & 7);
        if (allocated) {
            bitmap[page_index / 8] |= mask;
        } else {
            bitmap[page_index / 8] &= ~mask;
        }
    }

    fn findFirstFit(self: *const IovaAllocator, first_start: usize, start_limit: usize, page_count: usize) ?usize {
        var candidate = first_start;
        while (candidate < start_limit and candidate + page_count <= iova_page_count) {
            var offset: usize = 0;
            while (offset < page_count and !self.bitIsSet(candidate + offset)) : (offset += 1) {}
            if (offset == page_count) return candidate;
            candidate += offset + 1;
        }
        return null;
    }

    pub fn alloc(self: *IovaAllocator, page_count: usize) ?u64 {
        if (self.bitmap == null or page_count == 0 or page_count > iova_page_count) return null;
        const start = self.findFirstFit(self.cursor, iova_page_count, page_count) orelse
            self.findFirstFit(0, self.cursor, page_count) orelse return null;
        var index: usize = 0;
        while (index < page_count) : (index += 1) self.setBit(start + index, true);
        self.used_pages += page_count;
        self.peak_pages = @max(self.peak_pages, self.used_pages);
        self.cursor = (start + page_count) % iova_page_count;
        return iova_window_start + @as(u64, @intCast(start)) * page_size;
    }

    pub fn free(self: *IovaAllocator, iova: u64, page_count: usize) bool {
        if (self.bitmap == null or page_count == 0 or
            iova < iova_window_start or (iova & (page_size - 1)) != 0) return false;
        const offset = iova - iova_window_start;
        const start: usize = @intCast(offset / page_size);
        if (start >= iova_page_count or page_count > iova_page_count - start) return false;
        var index: usize = 0;
        while (index < page_count) : (index += 1) {
            if (!self.bitIsSet(start + index)) return false;
        }
        index = 0;
        while (index < page_count) : (index += 1) self.setBit(start + index, false);
        self.used_pages -= page_count;
        return true;
    }
};

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
