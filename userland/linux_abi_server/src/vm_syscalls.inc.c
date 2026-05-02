static u64 page_up(u64 value) { return (value + PAGE_BYTES - 1) & ~(u64)(PAGE_BYTES - 1); }

static int vm_add_region(u64 start, u64 size, u64 prot) {
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used) {
            g_regions[i].used = 1; g_regions[i].start = start; g_regions[i].size = size; g_regions[i].prot = prot;
            return 1;
        }
    }
    return 0;
}

static int vm_find_free_region_slot(void) {
    for (u64 i = 0; i < VM_REGION_MAX; i++) if (!g_regions[i].used) return (int)i;
    return -1;
}

static int vm_split_at(u64 point) {
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used) continue;
        const u64 rs = g_regions[i].start;
        const u64 re = rs + g_regions[i].size;
        if (point <= rs || point >= re) continue;
        const int slot = vm_find_free_region_slot();
        if (slot < 0) return 0;
        g_regions[(u64)slot].used = 1;
        g_regions[(u64)slot].start = point;
        g_regions[(u64)slot].size = re - point;
        g_regions[(u64)slot].prot = g_regions[i].prot;
        g_regions[i].size = point - rs;
        return 1;
    }
    return 1;
}

static int vm_split_range_boundaries(u64 start, u64 size) {
    return vm_split_at(start) && vm_split_at(start + size);
}

static int vm_range_covered(u64 start, u64 size) {
    const u64 end = start + size;
    u64 cursor = start;
    while (cursor < end) {
        u64 best_end = cursor;
        for (u64 i = 0; i < VM_REGION_MAX; i++) {
            if (!g_regions[i].used) continue;
            const u64 rs = g_regions[i].start;
            const u64 re = rs + g_regions[i].size;
            if (rs <= cursor && re > best_end) best_end = re;
        }
        if (best_end == cursor) return 0;
        cursor = best_end;
    }
    return 1;
}

static void vm_protect_range(u64 start, u64 size, u64 prot) {
    const u64 end = start + size;
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used) continue;
        const u64 rs = g_regions[i].start;
        const u64 re = rs + g_regions[i].size;
        if (rs >= start && re <= end) g_regions[i].prot = prot;
    }
}

static void vm_remove_range(u64 start, u64 size) {
    const u64 end = start + size;
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used) continue;
        const u64 rs = g_regions[i].start;
        const u64 re = rs + g_regions[i].size;
        if (rs >= start && re <= end) g_regions[i].used = 0;
    }
}

static u64 normalize_linux_prot(u64 prot) {
    prot &= 0x7;
    if ((prot & 0x2) != 0) prot |= 0x1;
    if (prot == 0) prot = 0x1;
    return prot;
}

static u64 apply_target_pages(u64 start, u64 page_count, u64 prot, u64 (*fn)(u64, u64, u64)) {
    u64 done = 0;
    while (done < page_count) {
        const u64 chunk = min_u64(page_count - done, 64);
        const u64 status = fn(start + done * PAGE_BYTES, chunk, prot);
        if (status != SYSCALL_OK) return status;
        done += chunk;
    }
    return SYSCALL_OK;
}

static int unmap_tracked_target_range(u64 start, u64 size) {
    u64 done = 0;
    const u64 page_count = size / PAGE_BYTES;
    while (done < page_count) {
        const u64 chunk = min_u64(page_count - done, 64);
        (void)unmap_reply_target_pages(start + done * PAGE_BYTES, chunk);
        done += chunk;
    }
    if (!vm_range_covered(start, size)) return 1;
    if (!vm_split_range_boundaries(start, size)) return 0;
    vm_remove_range(start, size);
    return 1;
}

static struct ipc_message handle_mmap(const struct trap_request *req) {
    enum { PROT_READ = 0x1, PROT_WRITE = 0x2, PROT_EXEC = 0x4, MAP_SHARED = 0x01, MAP_PRIVATE = 0x02, MAP_SHARED_VALIDATE = 0x03, MAP_TYPE = 0x0F, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20, MAP_FIXED_NOREPLACE = 0x100000 };
    const u64 requested_va = req->args[0]; const u64 len = req->args[1]; u64 prot = req->args[2] & (PROT_READ | PROT_WRITE | PROT_EXEC); const u64 flags = req->args[3]; const u64 fd = req->args[4]; const u64 offset = req->args[5]; const u64 map_type = flags & MAP_TYPE;
    const int file_backed = (flags & MAP_ANONYMOUS) == 0;
    if (len == 0) return reply(errno_inval(), 0); if (map_type != MAP_PRIVATE && map_type != MAP_SHARED && map_type != MAP_SHARED_VALIDATE) return reply(errno_inval(), 0); if ((offset & (PAGE_BYTES - 1)) != 0) return reply(errno_inval(), 0); if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0 && (requested_va == 0 || (requested_va & (PAGE_BYTES - 1)) != 0)) return reply(errno_inval(), 0); if (file_backed && (!fd_valid(fd) || g_fds[fd].kind != FD_FILE)) return reply(errno_badf(), 0); prot = normalize_linux_prot(prot);
    const u64 size = page_up(len); const u64 page_count = size / PAGE_BYTES; const u64 target_va = ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0) ? requested_va : g_mmap_next_va; const u64 map_prot = file_backed ? (prot | PROT_WRITE) : prot;
    if ((flags & MAP_FIXED_NOREPLACE) != 0 && vm_range_covered(target_va, size)) return reply(errno_exist(), 0);
    if ((flags & MAP_FIXED) != 0 && !unmap_tracked_target_range(target_va, size)) return reply(errno_nomem(), 0);
    const u64 map_status = map_reply_target_pages(target_va, page_count, map_prot);
    if (map_status == SYSCALL_OK) {
        if (file_backed) {
            int fault = 0;
            (void)read_fd_at_to_target(&g_fds[fd], offset, target_va, len, &fault);
            if (fault) return reply(errno_fault(), 0);
        }
        if (!vm_add_region(target_va, size, prot)) return reply(errno_nomem(), 0);
        if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) == 0) g_mmap_next_va += size;
        return reply(target_va, 0);
    }
    return reply(errno_nomem(), 0);
}
static struct ipc_message handle_brk(const struct trap_request *req) {
    if (req->args[0] == 0) return reply(g_brk_next_va, 0);
    if (req->args[0] > g_brk_next_va) {
        u64 from = page_up(g_brk_next_va);
        u64 to = page_up(req->args[0]);
        if (to > from) {
            if (map_reply_target_pages(from, (to - from) / PAGE_BYTES, 0x3) != SYSCALL_OK) return reply(g_brk_next_va, 0);
            if (!vm_add_region(from, to - from, 0x3)) return reply(g_brk_next_va, 0);
        }
    }
    g_brk_next_va = req->args[0];
    return reply(g_brk_next_va, 0);
}

static struct ipc_message handle_mprotect(const struct trap_request *req) {
    const u64 start = req->args[0]; const u64 len = req->args[1]; const u64 prot = normalize_linux_prot(req->args[2]);
    if (len == 0) return reply(0, 0);
    if ((start & (PAGE_BYTES - 1)) != 0) return reply(errno_inval(), 0);
    const u64 size = page_up(len);
    const int tracked = vm_range_covered(start, size);
    if (tracked && !vm_split_range_boundaries(start, size)) return reply(errno_nomem(), 0);
    const u64 status = apply_target_pages(start, size / PAGE_BYTES, prot, protect_reply_target_pages);
    if (status != SYSCALL_OK) return reply(errno_nomem(), 0);
    if (tracked) vm_protect_range(start, size, prot);
    return reply(0, 0);
}

static struct ipc_message handle_munmap(const struct trap_request *req) {
    const u64 start = req->args[0]; const u64 len = req->args[1];
    if (len == 0) return reply(errno_inval(), 0);
    if ((start & (PAGE_BYTES - 1)) != 0) return reply(errno_inval(), 0);
    const u64 size = page_up(len);
    if (!vm_range_covered(start, size)) return reply(errno_inval(), 0);
    if (!vm_split_range_boundaries(start, size)) return reply(errno_nomem(), 0);
    u64 done = 0; const u64 page_count = size / PAGE_BYTES;
    while (done < page_count) {
        const u64 chunk = min_u64(page_count - done, 64);
        const u64 status = unmap_reply_target_pages(start + done * PAGE_BYTES, chunk);
        if (status != SYSCALL_OK) return reply(errno_inval(), 0);
        done += chunk;
    }
    vm_remove_range(start, size);
    return reply(0, 0);
}
