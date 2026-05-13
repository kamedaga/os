const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_map_mmio: u64 = 0xB;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_map_page_anywhere: u64 = 0x5C;
const syscall_alloc_map_pages_anywhere: u64 = 0x5D;

pub const page_bytes: usize = 4096;
pub const syscall_ok: u64 = 0;

const dynamic_map_base_va: usize = 0x2300_0000;
const dynamic_map_end_va: usize = 0x2800_0000;

var next_dynamic_map_va: usize = dynamic_map_base_va;

fn syscall0(nr: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall2(nr: u64, arg0: u64, arg1: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall3(nr: u64, arg0: u64, arg1: u64, arg2: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
          [arg2] "{rdx}" (arg2),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn syscall4(nr: u64, arg0: u64, arg1: u64, arg2: u64, arg3: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (nr),
          [arg0] "{rdi}" (arg0),
          [arg1] "{rsi}" (arg1),
          [arg2] "{rdx}" (arg2),
          [arg3] "{rcx}" (arg3),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

pub const MappedPage = struct {
    va: usize,
    paddr: u64,
};

pub fn reservePages(page_count: usize) ?usize {
    if (page_count == 0) return null;
    const bytes = page_count * page_bytes;
    if (bytes / page_bytes != page_count) return null;
    const base = next_dynamic_map_va;
    const end = base + bytes;
    if (end < base or end > dynamic_map_end_va) return null;
    next_dynamic_map_va = end;
    return base;
}

pub fn allocMapPage(writable: bool) ?MappedPage {
    const paddr = syscall0(syscall_alloc_page);
    if (paddr < 0x1000) return null;
    return mapPageAtDynamicVa(paddr, writable);
}

pub fn allocMapPages(page_count: usize, writable: bool) ?usize {
    const va = reservePages(page_count) orelse return null;
    const flags: u64 = if (writable) 1 else 0;
    if (syscall4(syscall_alloc_map_pages, @intCast(va), @intCast(page_count), flags, 0) != syscall_ok) return null;
    return va;
}

pub fn allocMapPagesAnywhere(page_count: usize, writable: bool) ?usize {
    if (page_count == 0) return null;
    const flags: u64 = if (writable) 1 else 0;
    const va = syscall3(syscall_alloc_map_pages_anywhere, @intCast(page_count), flags, 0);
    if (va < page_bytes) return null;
    return @intCast(va);
}

pub fn allocMapPagesAnywhereInto(page_count: usize, writable: bool, paddrs: []u64) ?usize {
    if (page_count == 0 or page_count > paddrs.len) return null;
    var i: usize = 0;
    while (i < paddrs.len) : (i += 1) paddrs[i] = 0;
    const flags: u64 = if (writable) 1 else 0;
    const va = syscall3(syscall_alloc_map_pages_anywhere, @intCast(page_count), flags, @intFromPtr(paddrs.ptr));
    if (va < page_bytes) return null;
    return @intCast(va);
}

pub fn mapPageAtDynamicVa(paddr: u64, writable: bool) ?MappedPage {
    if (paddr < 0x1000) return null;
    const va = reservePages(1) orelse return null;
    if (!mapPageAtVa(va, paddr, writable)) return null;
    return .{ .va = va, .paddr = paddr };
}

pub fn mapPageAnywhere(paddr: u64, writable: bool) ?MappedPage {
    if (paddr < 0x1000) return null;
    const flags: u64 = if (writable) 1 else 0;
    const va = syscall2(syscall_map_page_anywhere, paddr, flags);
    if (va < page_bytes) return null;
    return .{ .va = @intCast(va), .paddr = paddr };
}

pub fn mapPageAtVa(va: usize, paddr: u64, writable: bool) bool {
    if (paddr < 0x1000) return false;
    const flags: u64 = if (writable) 1 else 0;
    return syscall3(syscall_map_page, @intCast(va), paddr, flags) == syscall_ok;
}

pub fn mapMmioPageAtVa(va: usize, paddr: u64, writable: bool) bool {
    if (paddr < 0x1000) return false;
    const flags: u64 = if (writable) 1 else 0;
    return syscall3(syscall_map_mmio, @intCast(va), paddr, flags) == syscall_ok;
}

pub fn mapPagesAtDynamicVa(paddrs: []const u64, writable: bool) ?usize {
    if (paddrs.len == 0) return null;
    const va = reservePages(paddrs.len) orelse return null;
    var index: usize = 0;
    while (index < paddrs.len) : (index += 1) {
        if (!mapPageAtVa(va + index * page_bytes, paddrs[index], writable)) return null;
    }
    return va;
}

pub fn allocMapPagesInto(page_count: usize, writable: bool, paddrs: []u64) ?usize {
    if (page_count == 0 or page_count > paddrs.len) return null;
    var i: usize = 0;
    while (i < paddrs.len) : (i += 1) paddrs[i] = 0;
    const va = reservePages(page_count) orelse return null;
    const flags: u64 = if (writable) 1 else 0;
    if (syscall4(syscall_alloc_map_pages, @intCast(va), @intCast(page_count), flags, @intFromPtr(paddrs.ptr)) != syscall_ok) return null;
    return va;
}
