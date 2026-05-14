static u64 page_up(u64 value) { return (value + PAGE_BYTES - 1) & ~(u64)(PAGE_BYTES - 1); }
static u64 page_down(u64 value) { return value & ~(u64)(PAGE_BYTES - 1); }

static void vm_merge_adjacent_regions(void) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (u64 i = 0; i < VM_REGION_MAX; i++) {
            if (!g_regions[i].used) continue;
            const u64 i_end = g_regions[i].start + g_regions[i].size;
            for (u64 j = 0; j < VM_REGION_MAX; j++) {
                if (i == j || !g_regions[j].used || g_regions[i].prot != g_regions[j].prot) continue;
                if (i_end == g_regions[j].start) {
                    g_regions[i].size += g_regions[j].size;
                    g_regions[j].used = 0;
                    changed = 1;
                    break;
                }
                const u64 j_end = g_regions[j].start + g_regions[j].size;
                if (j_end == g_regions[i].start) {
                    g_regions[i].start = g_regions[j].start;
                    g_regions[i].size += g_regions[j].size;
                    g_regions[j].used = 0;
                    changed = 1;
                    break;
                }
            }
            if (changed) break;
        }
    }
}

static int vm_add_region(u64 start, u64 size, u64 prot) {
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used || g_regions[i].prot != prot) continue;
        if (g_regions[i].start + g_regions[i].size == start) {
            g_regions[i].size += size;
            vm_merge_adjacent_regions();
            return 1;
        }
        if (start + size == g_regions[i].start) {
            g_regions[i].start = start;
            g_regions[i].size += size;
            vm_merge_adjacent_regions();
            return 1;
        }
    }
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used) {
            g_regions[i].used = 1; g_regions[i].start = start; g_regions[i].size = size; g_regions[i].prot = prot;
            vm_merge_adjacent_regions();
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

static int vm_range_prot(u64 start, u64 size, u64 *prot_out) {
    const u64 end = start + size;
    u64 cursor = start;
    int have_prot = 0;
    u64 prot = 0;
    while (cursor < end) {
        u64 best_end = cursor;
        u64 best_prot = 0;
        for (u64 i = 0; i < VM_REGION_MAX; i++) {
            if (!g_regions[i].used) continue;
            const u64 rs = g_regions[i].start;
            const u64 re = rs + g_regions[i].size;
            if (rs <= cursor && re > best_end) {
                best_end = re;
                best_prot = g_regions[i].prot;
            }
        }
        if (best_end == cursor) return 0;
        if (!have_prot) {
            prot = best_prot;
            have_prot = 1;
        } else if (prot != best_prot) {
            return 0;
        }
        cursor = best_end;
    }
    *prot_out = prot;
    return have_prot;
}

static int vm_range_uncovered(u64 start, u64 size) {
    if (size == 0) return 1;
    const u64 end = start + size;
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used) continue;
        const u64 rs = g_regions[i].start;
        const u64 re = rs + g_regions[i].size;
        if (start < re && end > rs) return 0;
    }
    return 1;
}

static u64 vm_first_overlap_end(u64 start, u64 size) {
    const u64 end = start + size;
    u64 next = 0;
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used) continue;
        const u64 rs = g_regions[i].start;
        const u64 re = rs + g_regions[i].size;
        if (start >= re || end <= rs) continue;
        if (next == 0 || re < next) next = re;
    }
    return next;
}

static u64 mmap_limit_exclusive(void) {
    const u64 reserve_before_brk = 0x01000000ULL;
    if (g_brk_next_va > reserve_before_brk) return page_down(g_brk_next_va - reserve_before_brk);
    return page_down(g_brk_next_va);
}

static u64 find_mmap_area(u64 size) {
    const u64 base = 0x29000000ULL;
    const u64 limit = mmap_limit_exclusive();
    if (size == 0 || limit <= base || size > limit - base) return 0;
    for (u64 pass = 0; pass < 2; pass++) {
        u64 candidate = page_up(pass == 0 && g_mmap_next_va > base ? g_mmap_next_va : base);
        while (candidate >= base && candidate <= limit - size) {
            const u64 overlap_end = vm_first_overlap_end(candidate, size);
            if (overlap_end == 0) return candidate;
            candidate = page_up(overlap_end);
        }
    }
    return 0;
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

static u64 map_target_pages_chunked(u64 start, u64 page_count, u64 prot) {
    if (page_count <= 64) return map_reply_target_pages(start, page_count, prot);
    return apply_target_pages(start, page_count, prot, map_reply_target_pages);
}

static u64 vm_page_count_bucket(u64 page_count) {
    if (page_count <= 1) return 0;
    if (page_count <= 4) return 1;
    if (page_count <= 16) return 2;
    if (page_count <= 64) return 3;
    return 4;
}

static void vm_trace4(const char *event, u64 a, u64 b, u64 c, u64 d) {
    if (!profile_trace_enabled()) return;
    profile_trace_prefix(event);
    user_log(" a=");
    user_log_hex_inline(a);
    user_log(" b=");
    user_log_hex_inline(b);
    user_log(" c=");
    user_log_hex_inline(c);
    user_log(" d=");
    user_log_hex_inline(d);
    user_log("\n");
}

static void sync_thread_group_vm_state(void) {
    if (!g_proc || g_proc->pid == 0) return;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *peer = &g_processes[i];
        if (!peer->used || peer == g_proc || peer->exec_pending || peer->principal == 0 || peer->pid != g_proc->pid) continue;
        peer->mmap_next_va = g_proc->mmap_next_va;
        peer->brk_next_va = g_proc->brk_next_va;
        for (u64 r = 0; r < VM_REGION_MAX; r++) peer->regions[r] = g_proc->regions[r];
    }
}

static u64 share_target_pages_to_thread_group(u64 start, u64 page_count, u64 prot) {
    if (!g_proc || g_proc->pid == 0) return SYSCALL_OK;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *peer = &g_processes[i];
        if (!peer->used || peer == g_proc || peer->exec_pending || peer->principal == 0 || peer->pid != g_proc->pid) continue;
        u64 done = 0;
        while (done < page_count) {
            const u64 chunk = min_u64(page_count - done, 64);
            const u64 status = share_reply_target_pages_to_trap_target(peer->principal, start + done * PAGE_BYTES, chunk, prot);
            if (status != SYSCALL_OK) return status;
            done += chunk;
        }
    }
    return SYSCALL_OK;
}

static u64 unmap_target_pages_from_thread_group(u64 start, u64 page_count) {
    if (!g_proc || g_proc->pid == 0) return SYSCALL_OK;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *peer = &g_processes[i];
        if (!peer->used || peer == g_proc || peer->exec_pending || peer->principal == 0 || peer->pid != g_proc->pid) continue;
        u64 done = 0;
        while (done < page_count) {
            const u64 chunk = min_u64(page_count - done, 64);
            const u64 status = unmap_trap_target_pages(peer->principal, start + done * PAGE_BYTES, chunk);
            if (status != SYSCALL_OK) return status;
            done += chunk;
        }
    }
    return SYSCALL_OK;
}

static int unmap_tracked_target_range(u64 start, u64 size) {
    vm_trace4("vm.unmap_tracked.begin", start, size, 0, 0);
    u64 done = 0;
    const u64 page_count = size / PAGE_BYTES;
    while (done < page_count) {
        const u64 chunk = min_u64(page_count - done, 64);
        const u64 status = unmap_reply_target_pages(start + done * PAGE_BYTES, chunk);
        if (status != SYSCALL_OK) {
            vm_trace4("vm.unmap_tracked.current_fail", start + done * PAGE_BYTES, chunk, status, 0);
            return 0;
        }
        done += chunk;
    }
    const u64 group_status = unmap_target_pages_from_thread_group(start, page_count);
    if (group_status != SYSCALL_OK) {
        vm_trace4("vm.unmap_tracked.group_fail", start, page_count, group_status, 0);
        return 0;
    }
    if (!vm_range_covered(start, size)) return 1;
    if (!vm_split_range_boundaries(start, size)) return 0;
    vm_remove_range(start, size);
    sync_thread_group_vm_state();
    vm_trace4("vm.unmap_tracked.done", start, size, page_count, 0);
    return 1;
}

static int unmap_overlapping_target_range(u64 start, u64 size) {
    const u64 end = start + size;
    for (;;) {
        u64 overlap_start = 0;
        u64 overlap_end = 0;
        for (u64 i = 0; i < VM_REGION_MAX; i++) {
            if (!g_regions[i].used) continue;
            const u64 rs = g_regions[i].start;
            const u64 re = rs + g_regions[i].size;
            if (start >= re || end <= rs) continue;
            overlap_start = rs > start ? rs : start;
            overlap_end = re < end ? re : end;
            break;
        }
        if (overlap_end <= overlap_start) return 1;
        if (!unmap_tracked_target_range(overlap_start, overlap_end - overlap_start)) return 0;
    }
}

static struct ipc_message handle_mmap(const struct trap_request *req) {
    enum { PROT_READ = 0x1, PROT_WRITE = 0x2, PROT_EXEC = 0x4, MAP_SHARED = 0x01, MAP_PRIVATE = 0x02, MAP_SHARED_VALIDATE = 0x03, MAP_TYPE = 0x0F, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20, MAP_FIXED_NOREPLACE = 0x100000 };
    const u64 requested_va = req->args[0]; const u64 len = req->args[1]; u64 prot = req->args[2] & (PROT_READ | PROT_WRITE | PROT_EXEC); const u64 flags = req->args[3]; const u64 fd = req->args[4]; const u64 offset = req->args[5]; const u64 map_type = flags & MAP_TYPE;
    const int file_backed = (flags & MAP_ANONYMOUS) == 0;
    if (len == 0) return reply(errno_inval(), 0); if (map_type != MAP_PRIVATE && map_type != MAP_SHARED && map_type != MAP_SHARED_VALIDATE) return reply(errno_inval(), 0); if ((offset & (PAGE_BYTES - 1)) != 0) return reply(errno_inval(), 0); if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0 && (requested_va == 0 || (requested_va & (PAGE_BYTES - 1)) != 0)) return reply(errno_inval(), 0); if (file_backed && (!fd_valid(fd) || g_fds[fd].kind != FD_FILE)) return reply(errno_badf(), 0); prot = normalize_linux_prot(prot);
    const u64 size = page_up(len); const u64 page_count = size / PAGE_BYTES; u64 target_va = ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0) ? requested_va : find_mmap_area(size); const u64 map_prot = file_backed ? ((prot | PROT_WRITE) & ~PROT_EXEC) : prot;
    vm_trace4("vm.mmap.begin", requested_va, len, prot, flags);
    if (target_va == 0) return reply(errno_nomem(), 0);
    g_prof.mmap_calls++;
    g_prof.mmap_pages += page_count;
    const u64 mmap_bucket = vm_page_count_bucket(page_count);
    g_prof.mmap_bucket_calls[mmap_bucket]++;
    g_prof.mmap_bucket_pages[mmap_bucket] += page_count;
    if (file_backed) {
        g_prof.mmap_file_calls++;
        g_prof.mmap_file_pages += page_count;
        g_prof.mmap_file_bytes += len;
    }
    if ((flags & MAP_FIXED_NOREPLACE) != 0 && vm_range_covered(target_va, size)) return reply(errno_exist(), 0);
    if ((flags & MAP_FIXED) != 0 && !unmap_overlapping_target_range(target_va, size)) return reply(errno_nomem(), 0);
    u64 map_status = map_target_pages_chunked(target_va, page_count, map_prot);
    if (map_status == SYSCALL_ERR_MAP && (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) == 0) {
        g_mmap_next_va = target_va + size;
        target_va = find_mmap_area(size);
        if (target_va != 0) map_status = map_target_pages_chunked(target_va, page_count, map_prot);
    }
    if (map_status == SYSCALL_OK) {
        if (file_backed) {
            int fault = 0;
            (void)read_fd_at_to_target(&g_fds[fd], offset, target_va, len, &fault);
            if (fault) return reply(errno_fault(), 0);
            if (map_prot != prot) {
                const u64 protect_status = apply_target_pages(target_va, page_count, prot, protect_reply_target_pages);
                if (protect_status != SYSCALL_OK) {
                    user_log("LinuxAbiServer: mmap file-backed protect failed\n");
                    user_log_hex_value(protect_status);
                    return reply(errno_nomem(), 0);
                }
            }
        }
        const u64 share_prot = file_backed ? prot : map_prot;
        const u64 share_status = share_target_pages_to_thread_group(target_va, page_count, share_prot);
        if (share_status != SYSCALL_OK) {
            user_log("LinuxAbiServer: mmap thread-group share failed\n");
            user_log_hex_value(share_status);
            return reply(errno_nomem(), 0);
        }
        if (!vm_add_region(target_va, size, prot)) {
            user_log("LinuxAbiServer: mmap vm_add_region failed\n");
            user_log_hex_value(target_va);
            user_log_hex_value(size);
            return reply(errno_nomem(), 0);
        }
        if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) == 0) g_mmap_next_va = target_va + size;
        sync_thread_group_vm_state();
        vm_trace4("vm.mmap.ok", target_va, size, prot, flags);
        return reply(target_va, 0);
    }
    user_log("LinuxAbiServer: mmap map_reply_target_pages failed\n");
    user_log_hex_value(map_status);
    user_log_hex_value(target_va);
    user_log_hex_value(page_count);
    return reply(errno_nomem(), 0);
}

static struct ipc_message handle_mremap(const struct trap_request *req) {
    enum { MREMAP_MAYMOVE = 0x1, MREMAP_FIXED = 0x2, MREMAP_DONTUNMAP = 0x4 };
    const u64 old_addr = req->args[0];
    const u64 old_len = req->args[1];
    const u64 new_len = req->args[2];
    const u64 flags = req->args[3];
    const u64 requested_new_addr = req->args[4];
    vm_trace4("vm.mremap.begin", old_addr, old_len, new_len, flags);
    if ((flags & ~(u64)(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP)) != 0) return reply(errno_inval(), 0);
    if ((flags & MREMAP_FIXED) != 0 && (flags & MREMAP_MAYMOVE) == 0) return reply(errno_inval(), 0);
    if ((flags & MREMAP_DONTUNMAP) != 0) return reply(errno_inval(), 0);
    if (old_len == 0 || new_len == 0 || (old_addr & (PAGE_BYTES - 1)) != 0) return reply(errno_inval(), 0);
    const u64 old_size = page_up(old_len);
    const u64 new_size = page_up(new_len);
    if (!vm_range_covered(old_addr, old_size)) return reply(errno_fault(), 0);
    u64 prot = 0;
    if (!vm_range_prot(old_addr, old_size, &prot)) return reply(errno_nomem(), 0);

    if (new_size <= old_size) {
        const u64 tail = old_size - new_size;
        if (tail != 0 && !unmap_tracked_target_range(old_addr + new_size, tail)) return reply(errno_nomem(), 0);
        vm_trace4("vm.mremap.shrink_ok", old_addr, old_size, new_size, flags);
        return reply(old_addr, 0);
    }

    const u64 extra_size = new_size - old_size;
    const u64 grow_addr = old_addr + old_size;
    if ((flags & MREMAP_FIXED) == 0 && vm_range_uncovered(grow_addr, extra_size)) {
        const u64 extra_pages = extra_size / PAGE_BYTES;
        const u64 status = map_target_pages_chunked(grow_addr, extra_pages, prot);
        if (status == SYSCALL_OK) {
            const u64 share_status = share_target_pages_to_thread_group(grow_addr, extra_pages, prot);
            if (share_status != SYSCALL_OK) {
                (void)unmap_tracked_target_range(grow_addr, extra_size);
                return reply(errno_nomem(), 0);
            }
            if (!vm_add_region(grow_addr, extra_size, prot)) {
                (void)unmap_tracked_target_range(grow_addr, extra_size);
                return reply(errno_nomem(), 0);
            }
            sync_thread_group_vm_state();
            vm_trace4("vm.mremap.grow_ok", old_addr, old_size, new_size, flags);
            return reply(old_addr, 0);
        }
    }

    /* Moving mremap needs an atomic target-VM remap primitive. Failing without
       touching the old mapping lets libc fall back to malloc/copy/free safely. */
    (void)requested_new_addr;
    return reply(errno_nomem(), 0);
}

static struct ipc_message handle_brk(const struct trap_request *req) {
    g_prof.brk_calls++;
    if (req->args[0] == 0) return reply(g_brk_next_va, 0);
    if (req->args[0] > g_brk_next_va) {
        u64 from = page_up(g_brk_next_va);
        u64 to = page_up(req->args[0]);
        if (to > from) {
            const u64 status = map_target_pages_chunked(from, (to - from) / PAGE_BYTES, 0x3);
            if (status != SYSCALL_OK) return reply(g_brk_next_va, 0);
            const u64 share_status = share_target_pages_to_thread_group(from, (to - from) / PAGE_BYTES, 0x3);
            if (share_status != SYSCALL_OK) {
                user_log("LinuxAbiServer: brk thread-group share failed\n");
                user_log_hex_value(share_status);
                return reply(g_brk_next_va, 0);
            }
            if (!vm_add_region(from, to - from, 0x3)) {
                user_log("LinuxAbiServer: brk vm_add_region failed\n");
                user_log_hex_value(from);
                user_log_hex_value(to - from);
                return reply(g_brk_next_va, 0);
            }
        }
    }
    g_brk_next_va = req->args[0];
    sync_thread_group_vm_state();
    return reply(g_brk_next_va, 0);
}

static struct ipc_message handle_mprotect(const struct trap_request *req) {
    const u64 start = req->args[0]; const u64 len = req->args[1]; const u64 prot = normalize_linux_prot(req->args[2]);
    if (len == 0) return reply(0, 0);
    if ((start & (PAGE_BYTES - 1)) != 0) return reply(errno_inval(), 0);
    const u64 size = page_up(len);
    vm_trace4("vm.mprotect.begin", start, size, prot, 0);
    g_prof.mprotect_calls++;
    g_prof.mprotect_pages += size / PAGE_BYTES;
    const int tracked = vm_range_covered(start, size);
    if (tracked && !vm_split_range_boundaries(start, size)) return reply(errno_nomem(), 0);
    const u64 status = apply_target_pages(start, size / PAGE_BYTES, prot, protect_reply_target_pages);
    if (!tracked && status == SYSCALL_ERR_MAP) return reply(0, 0);
    if (status != SYSCALL_OK) {
        user_log("LinuxAbiServer: mprotect current target failed\n");
        user_log_hex_value(status);
        user_log_hex_value(start);
        user_log_hex_value(size / PAGE_BYTES);
        user_log_hex_value(prot);
        return reply(errno_nomem(), 0);
    }
    const u64 share_status = share_target_pages_to_thread_group(start, size / PAGE_BYTES, prot);
    if (share_status != SYSCALL_OK) {
        user_log("LinuxAbiServer: mprotect thread-group share failed\n");
        user_log_hex_value(share_status);
        return reply(errno_nomem(), 0);
    }
    if (tracked) vm_protect_range(start, size, prot);
    sync_thread_group_vm_state();
    vm_trace4("vm.mprotect.done", start, size, prot, 0);
    return reply(0, 0);
}

static struct ipc_message handle_munmap(const struct trap_request *req) {
    const u64 start = req->args[0]; const u64 len = req->args[1];
    if (len == 0) return reply(errno_inval(), 0);
    if ((start & (PAGE_BYTES - 1)) != 0) return reply(errno_inval(), 0);
    const u64 size = page_up(len);
    const u64 page_count = size / PAGE_BYTES;
    vm_trace4("vm.munmap.begin", start, len, size, 0);
    g_prof.munmap_calls++;
    g_prof.munmap_pages += page_count;
    const u64 munmap_bucket = vm_page_count_bucket(page_count);
    g_prof.munmap_bucket_calls[munmap_bucket]++;
    g_prof.munmap_bucket_pages[munmap_bucket] += page_count;
    const int ok = unmap_overlapping_target_range(start, size);
    vm_trace4("vm.munmap.done", start, size, ok ? 0 : errno_nomem(), 0);
    return reply(ok ? 0 : errno_nomem(), 0);
}
