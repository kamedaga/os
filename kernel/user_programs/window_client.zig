const syscall_create_window: u64 = 0xD;
const syscall_map_page: u64 = 0x2;
const syscall_send_cap: u64 = 0x6;
const syscall_grant_cap: u64 = 0x8;
const syscall_grant_caps_batch: u64 = 0x14;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_log: u64 = 0x9;
const syscall_untyped_alloc_map_pages: u64 = 0x13;
const protocol = @import("window_protocol.zig");

const syscall_ok: u64 = 0;
pub const endpoint_to_process1: u64 = 0x11;
pub const window_cap_magic = protocol.window_cap_magic;
pub const window_meta_magic = protocol.window_meta_magic;
pub const window_flag_allow_pixels_dma = protocol.window_flag_allow_pixels_dma;
pub const window_flag_low_scale = protocol.window_flag_low_scale;
pub const WindowCap = protocol.WindowCap;
pub const WindowMeta = protocol.WindowMeta;
const max_window_pixel_pages: usize = 128;
const large_window_pixel_pages_threshold: usize = 8;
const untyped_alloc_map_writable_flag: u64 = 1 << 0;
const untyped_alloc_map_drop_cap_after_map_flag: u64 = 1 << 1;
const untyped_alloc_map_contiguous_flag: u64 = 1 << 2;
const untyped_alloc_map_dma_ok_flag: u64 = 1 << 3;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn userLogHex(label: []const u8, value: u64) void {
    var buf: [80]u8 = undefined;
    var len: usize = 0;
    while (len < label.len and len < buf.len) : (len += 1) {
        buf[len] = label[len];
    }
    if (len + 3 >= buf.len) return;
    buf[len] = '0';
    buf[len + 1] = 'x';
    len += 2;
    var shift: u6 = 60;
    while (true) {
        const nibble: u8 = @intCast((value >> shift) & 0xF);
        buf[len] = if (nibble < 10) '0' + nibble else 'A' + (nibble - 10);
        len += 1;
        if (shift == 0) break;
        shift -= 4;
    }
    buf[len] = '\n';
    len += 1;
    _ = userLog(buf[0..len]);
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn sendCap(paddr: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_send_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantCap(paddr: u64, to: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (to),
          [arg2] "{rdx}" (rights_bits),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantCapsBatch(paddr_list_va: u64, page_count: u64, to: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_caps_batch),
          [arg0] "{rdi}" (paddr_list_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (to),
          [arg3] "{rcx}" (rights_bits),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn allocMapPages(base_va: u64, page_count: u64, writable: bool, out_paddr_list_va: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_map_pages),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
          [arg3] "{rcx}" (out_paddr_list_va),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn allocUntypedMapPages(base_va: u64, page_count: u64, writable: bool, dma_ok: bool, out_paddr_list_va: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_untyped_alloc_map_pages),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (@as(u64, if (writable) untyped_alloc_map_writable_flag else 0) |
            untyped_alloc_map_contiguous_flag |
            @as(u64, if (dma_ok) untyped_alloc_map_dma_ok_flag else 0)),
          [arg3] "{rcx}" (out_paddr_list_va),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn createWindowSys(width: u16, height: u16, flags: u32) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_create_window),
          [arg0] "{rdi}" (@as(u64, width)),
          [arg1] "{rsi}" (@as(u64, height)),
          [arg2] "{rdx}" (@as(u64, flags)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

pub fn createAndPublishWindow(
    width: u16,
    height: u16,
    flags: u32,
    cap_tmp_va: u64,
    pixel_map_va: u64,
    meta_map_va: u64,
) bool {
    const effective_flags = flags | window_flag_allow_pixels_dma;
    const cap_paddr = createWindowSys(width, height, effective_flags);
    if (cap_paddr < 0x1000) {
        _ = userLog("window_client: create_window failed\n");
        return false;
    }
    if (mapPage(cap_tmp_va, cap_paddr, true) != syscall_ok) {
        _ = userLog("window_client: map cap page failed\n");
        return false;
    }
    const cap: *volatile WindowCap = @ptrFromInt(cap_tmp_va);
    if (cap.magic != window_cap_magic or cap.version != 1) {
        _ = userLog("window_client: bad window cap magic\n");
        userLogHex("window_client: cap_paddr=", cap_paddr);
        userLogHex("window_client: cap_magic=", cap.magic);
        userLogHex("window_client: cap_version=", cap.version);
        return false;
    }
    if (cap.pixels_page_count == 0) {
        _ = userLog("window_client: pixels_page_count == 0\n");
        return false;
    }
    const page_count: usize = @intCast(cap.pixels_page_count);
    if (page_count > max_window_pixel_pages) {
        _ = userLog("window_client: pixels_page_count too large\n");
        return false;
    }
    const cap_size = @sizeOf(WindowCap);
    const list_capacity = (4096 - cap_size) / @sizeOf(u64);
    if (page_count > list_capacity) {
        _ = userLog("window_client: cap page paddr list overflow\n");
        return false;
    }

    var pixel_paddrs: [max_window_pixel_pages]u64 = [_]u64{0} ** max_window_pixel_pages;
    const use_untyped = (effective_flags & window_flag_allow_pixels_dma) != 0 or page_count >= large_window_pixel_pages_threshold;
    const alloc_result = if (use_untyped)
        allocUntypedMapPages(pixel_map_va, @intCast(page_count), true, (effective_flags & window_flag_allow_pixels_dma) != 0, @intFromPtr(&pixel_paddrs))
    else
        allocMapPages(pixel_map_va, @intCast(page_count), true, @intFromPtr(&pixel_paddrs));
    if (alloc_result != syscall_ok) {
        _ = userLog("window_client: alloc/map pixels failed\n");
        userLogHex("window_client: alloc_result=", alloc_result);
        userLogHex("window_client: pixel_pages=", page_count);
        userLogHex("window_client: use_untyped=", if (use_untyped) 1 else 0);
        return false;
    }
    var i: usize = 0;
    while (i < page_count) : (i += 1) {
        const pixel_paddr = pixel_paddrs[i];
        if (pixel_paddr < 0x1000) {
            _ = userLog("window_client: bad pixels paddr\n");
            return false;
        }
    }
    var grant_rights: u64 = 0x1; // cpu_read
    if ((effective_flags & window_flag_allow_pixels_dma) != 0) {
        grant_rights |= 0x4; // dma
    }
    if (grantCapsBatch(@intFromPtr(&pixel_paddrs), @intCast(page_count), 1, grant_rights) != syscall_ok) {
        i = 0;
        while (i < page_count) : (i += 1) {
            if (grantCap(pixel_paddrs[i], 1, grant_rights) != syscall_ok) {
                _ = userLog("window_client: grant pixels cap failed\n");
                return false;
            }
        }
    }
    cap.pixels_cap_paddr = pixel_paddrs[0];
    const paddr_list: [*]volatile u64 = @ptrFromInt(cap_tmp_va + cap_size);
    i = 0;
    while (i < page_count) : (i += 1) {
        paddr_list[i] = pixel_paddrs[i];
    }
    if (mapPage(meta_map_va, cap.meta_cap_paddr, true) != syscall_ok) {
        _ = userLog("window_client: map meta failed\n");
        return false;
    }
    if (sendCap(cap_paddr, endpoint_to_process1) != syscall_ok) {
        _ = userLog("window_client: send window cap failed\n");
        return false;
    }
    return true;
}

pub fn createAndPublishWindowWithDma(
    width: u16,
    height: u16,
    cap_tmp_va: u64,
    pixel_map_va: u64,
    meta_map_va: u64,
) bool {
    return createAndPublishWindow(
        width,
        height,
        window_flag_allow_pixels_dma,
        cap_tmp_va,
        pixel_map_va,
        meta_map_va,
    );
}

pub fn setWindowTitle(meta_va: u64, title: []const u8) void {
    const meta: *volatile WindowMeta = @ptrFromInt(meta_va);
    if (meta.magic != window_meta_magic) return;
    const copy_len: usize = if (title.len < 63) title.len else 63;
    var i: usize = 0;
    while (i < 64) : (i += 1) {
        meta.title[i] = 0;
    }
    i = 0;
    while (i < copy_len) : (i += 1) {
        meta.title[i] = title[i];
    }
    meta.title_len = @intCast(copy_len);
    markWindowDirty(meta_va);
}

pub fn markWindowDirty(meta_va: u64) void {
    const meta: *volatile WindowMeta = @ptrFromInt(meta_va);
    if (meta.magic != window_meta_magic) return;
    meta.seq +%= 1;
}
