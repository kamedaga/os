static u64 page_up(u64 value) { return (value + PAGE_BYTES - 1) & ~(u64)(PAGE_BYTES - 1); }
static u64 page_down(u64 value) { return value & ~(u64)(PAGE_BYTES - 1); }

static int page_up_checked(u64 value, u64 *out) {
    if (value > ~0ULL - (PAGE_BYTES - 1)) return 0;
    *out = page_up(value);
    return 1;
}

static int linux_user_range_valid(u64 start, u64 size) {
    return layout_range_valid(g_user_low_va, g_user_top_va, start, size);
}

static u64 stack_reserved_low_va(void) {
    enum { LINUX_STACK_GUARD_PAGES = 16 };
    if (g_stack_top_va == 0) return 0;
    u64 stack_pages = g_stack_page_count == 0 ? USER_LAYOUT_DEFAULT_STACK_PAGES : g_stack_page_count;
    if (stack_pages > (~0ULL / PAGE_BYTES) - LINUX_STACK_GUARD_PAGES) return 0;
    const u64 reserve = (stack_pages + LINUX_STACK_GUARD_PAGES) * PAGE_BYTES;
    if (g_stack_top_va <= reserve) return 0;
    return page_down(g_stack_top_va - reserve);
}

static u64 brk_limit_exclusive(void) {
    u64 limit = g_user_top_va;
    if (g_mmap_base_va > g_brk_initial_va && g_mmap_base_va < limit) {
        limit = g_mmap_base_va;
    }
    const u64 stack_low = stack_reserved_low_va();
    if (stack_low > g_brk_initial_va && stack_low < limit) {
        limit = stack_low;
    }
    return page_down(limit);
}

static int vm_regions_can_merge(u64 left, u64 right) {
    if (left >= VM_REGION_MAX || right >= VM_REGION_MAX || !g_regions[left].used || !g_regions[right].used) return 0;
    if (g_regions[left].prot != g_regions[right].prot) return 0;
    if (g_regions[left].file_backed != g_regions[right].file_backed) return 0;
    if (g_regions[left].file_lazy != g_regions[right].file_lazy) return 0;
    if (g_regions[left].file_vm_object != g_regions[right].file_vm_object) return 0;
    if (!g_regions[left].file_backed) return 1;
    if (g_regions[left].file_token != g_regions[right].file_token) return 0;
    if (g_regions[left].file_size != g_regions[right].file_size) return 0;
    return g_regions[left].file_offset + g_regions[left].size == g_regions[right].file_offset;
}

static void vm_merge_region_at(u64 slot) {
    if (slot >= VM_REGION_MAX || !g_regions[slot].used) return;
    int changed = 1;
    while (changed) {
        changed = 0;
        const u64 slot_end = g_regions[slot].start + g_regions[slot].size;
        for (u64 j = 0; j < VM_REGION_MAX; j++) {
            if (slot == j || !g_regions[j].used) continue;
            if (slot_end == g_regions[j].start) {
                if (!vm_regions_can_merge(slot, j)) continue;
                g_regions[slot].size += g_regions[j].size;
                g_regions[j].used = 0;
                changed = 1;
                break;
            }
            const u64 j_end = g_regions[j].start + g_regions[j].size;
            if (j_end == g_regions[slot].start) {
                if (!vm_regions_can_merge(j, slot)) continue;
                g_regions[slot].start = g_regions[j].start;
                g_regions[slot].size += g_regions[j].size;
                if (g_regions[slot].file_backed) g_regions[slot].file_offset = g_regions[j].file_offset;
                g_regions[j].used = 0;
                changed = 1;
                break;
            }
        }
    }
}

static int vm_add_region(u64 start, u64 size, u64 prot, u64 file_token, u64 file_offset, u64 file_size, int file_backed, int file_lazy, int file_vm_object) {
    if (!linux_user_range_valid(start, size)) return 0;
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used || g_regions[i].prot != prot || g_regions[i].file_backed != (u8)file_backed || g_regions[i].file_lazy != (u8)file_lazy || g_regions[i].file_vm_object != (u8)file_vm_object) continue;
        if (file_backed && g_regions[i].file_token != file_token) continue;
        if (file_backed && g_regions[i].file_size != file_size) continue;
        if (g_regions[i].start + g_regions[i].size == start) {
            if (file_backed && g_regions[i].file_offset + g_regions[i].size != file_offset) continue;
            g_regions[i].size += size;
            vm_merge_region_at(i);
            return 1;
        }
        if (start + size == g_regions[i].start) {
            if (file_backed && file_offset + size != g_regions[i].file_offset) continue;
            g_regions[i].start = start;
            g_regions[i].size += size;
            g_regions[i].file_token = file_token;
            g_regions[i].file_offset = file_offset;
            g_regions[i].file_size = file_size;
            g_regions[i].file_backed = (u8)file_backed;
            g_regions[i].file_lazy = (u8)file_lazy;
            g_regions[i].file_vm_object = (u8)file_vm_object;
            vm_merge_region_at(i);
            return 1;
        }
    }
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used) {
            g_regions[i].used = 1; g_regions[i].start = start; g_regions[i].size = size; g_regions[i].prot = prot;
            g_regions[i].file_token = file_backed ? file_token : 0;
            g_regions[i].file_offset = file_backed ? file_offset : 0;
            g_regions[i].file_size = file_backed ? file_size : 0;
            g_regions[i].file_backed = (u8)file_backed;
            g_regions[i].file_lazy = (u8)file_lazy;
            g_regions[i].file_vm_object = (u8)file_vm_object;
            vm_merge_region_at(i);
            return 1;
        }
    }
    return 0;
}

static u64 vm_region_used_count(void) {
    u64 count = 0;
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (g_regions[i].used) count++;
    }
    return count;
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
        g_regions[(u64)slot].file_token = g_regions[i].file_token;
        g_regions[(u64)slot].file_offset = g_regions[i].file_backed ? g_regions[i].file_offset + (point - rs) : 0;
        g_regions[(u64)slot].file_size = g_regions[i].file_size;
        g_regions[(u64)slot].file_backed = g_regions[i].file_backed;
        g_regions[(u64)slot].file_lazy = g_regions[i].file_lazy;
        g_regions[(u64)slot].file_vm_object = g_regions[i].file_vm_object;
        g_regions[i].size = point - rs;
        return 1;
    }
    return 1;
}

static int vm_split_range_boundaries(u64 start, u64 size) {
    if (!linux_user_range_valid(start, size)) return 0;
    return vm_split_at(start) && vm_split_at(start + size);
}

static int vm_range_covered(u64 start, u64 size) {
    if (!linux_user_range_valid(start, size)) return 0;
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
    if (!linux_user_range_valid(start, size)) return 0;
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
    if (!linux_user_range_valid(start, size)) return 0;
    const u64 end = start + size;
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used) continue;
        const u64 rs = g_regions[i].start;
        const u64 re = rs + g_regions[i].size;
        if (start < re && end > rs) return 0;
    }
    return 1;
}

static int vm_range_has_file_backing(u64 start, u64 size, u64 file_token, u64 file_offset) {
    if (!linux_user_range_valid(start, size)) return 0;
    const u64 end = start + size;
    u64 cursor = start;
    while (cursor < end) {
        u64 best_end = cursor;
        for (u64 i = 0; i < VM_REGION_MAX; i++) {
            if (!g_regions[i].used || !g_regions[i].file_backed || g_regions[i].file_token != file_token) continue;
            const u64 rs = g_regions[i].start;
            const u64 re = rs + g_regions[i].size;
            if (rs > cursor || re <= cursor) continue;
            if (g_regions[i].file_offset + (cursor - rs) != file_offset + (cursor - start)) continue;
            best_end = re < end ? re : end;
            break;
        }
        if (best_end == cursor) return 0;
        cursor = best_end;
    }
    return 1;
}

static int vm_range_has_lazy_file_backing(u64 start, u64 size) {
    if (!linux_user_range_valid(start, size)) return 0;
    const u64 end = start + size;
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used || !g_regions[i].file_backed || !g_regions[i].file_lazy) continue;
        const u64 rs = g_regions[i].start;
        const u64 re = rs + g_regions[i].size;
        if (start < re && end > rs) return 1;
    }
    return 0;
}

static u64 vm_first_overlap_end(u64 start, u64 size) {
    if (!linux_user_range_valid(start, size)) return 0;
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

static u64 normalized_mmap_base(void) {
    u64 base = g_mmap_base_va;
    if (base < LINUX_MMAP_BASE_VA || base < g_user_low_va || base >= g_user_top_va) {
        base = LINUX_MMAP_BASE_VA;
    }
    return page_up(base);
}

static u64 mmap_limit_exclusive_for_base(u64 base) {
    u64 limit = g_user_top_va;
    if (limit == 0 || limit <= base) return 0;
    const u64 stack_low = stack_reserved_low_va();
    if (stack_low > base && stack_low < limit) {
        limit = stack_low;
    }
    if (g_brk_next_va > base && g_brk_next_va < limit) {
        limit = g_brk_next_va;
    }
    return page_down(limit);
}

static u64 find_mmap_area(u64 size) {
    const u64 base = normalized_mmap_base();
    const u64 limit = mmap_limit_exclusive_for_base(base);
    if (size == 0 || limit <= base || size > limit - base) return 0;
    for (u64 pass = 0; pass < 2; pass++) {
        u64 candidate = 0;
        if (!page_up_checked(pass == 0 && g_mmap_next_va > base ? g_mmap_next_va : base, &candidate)) return 0;
        while (candidate >= base && candidate <= limit - size) {
            const u64 overlap_end = vm_first_overlap_end(candidate, size);
            if (overlap_end == 0) return candidate;
            if (!page_up_checked(overlap_end, &candidate)) return 0;
        }
    }
    return 0;
}

static u64 find_mmap32_area(u64 size) {
    u64 base = 0x40000000ULL;
    const u64 hard_limit = 0x80000000ULL;
    u64 limit = g_user_top_va < hard_limit ? g_user_top_va : hard_limit;
    if (base < g_user_low_va) base = g_user_low_va;
    base = page_up(base);
    limit = page_down(limit);
    if (size == 0 || limit <= base || size > limit - base) return 0;
    u64 candidate = base;
    while (candidate >= base && candidate <= limit - size) {
        const u64 overlap_end = vm_first_overlap_end(candidate, size);
        if (overlap_end == 0) return candidate;
        if (!page_up_checked(overlap_end, &candidate)) return 0;
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

static u64 linux_prot_for_kernel_map(u64 prot) {
    /* Linux permits writable executable mappings. PachaOS does not enforce NX
       yet, and the kernel map API rejects W+X, so keep write access and clear
       the tracked exec bit only for the internal mapping syscall. */
    if ((prot & 0x2) != 0) prot &= ~0x4ULL;
    return prot;
}

static u64 apply_target_pages(u64 start, u64 page_count, u64 prot, u64 (*fn)(u64, u64, u64)) {
    u64 done = 0;
    const u64 kernel_prot = linux_prot_for_kernel_map(prot);
    while (done < page_count) {
        const u64 chunk = min_u64(page_count - done, 64);
        const u64 status = fn(start + done * PAGE_BYTES, chunk, kernel_prot);
        if (status != SYSCALL_OK) return status;
        done += chunk;
    }
    return SYSCALL_OK;
}

static int vm_apply_mapped_pages_in_range(u64 start, u64 size, u64 prot, u64 (*fn)(u64, u64, u64)) {
    const u64 end = start + size;
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used || g_regions[i].file_lazy) continue;
        const u64 rs = g_regions[i].start;
        const u64 re = rs + g_regions[i].size;
        if (rs < start || re > end) continue;
        const u64 status = apply_target_pages(rs, g_regions[i].size / PAGE_BYTES, prot, fn);
        if (status != SYSCALL_OK) return 0;
    }
    return 1;
}

static u64 map_target_pages_chunked(u64 start, u64 page_count, u64 prot) {
    if (page_count <= 64) return map_reply_target_pages(start, page_count, linux_prot_for_kernel_map(prot));
    return apply_target_pages(start, page_count, prot, map_reply_target_pages);
}

static int copy_zero_to_target_range(u64 dst, u64 len);

static u64 map_zeroed_target_pages_chunked(u64 start, u64 page_count, u64 prot) {
    const u64 map_prot = (prot | 0x2) & ~0x4;
    const u64 status = map_target_pages_chunked(start, page_count, map_prot);
    if (status != SYSCALL_OK) return status;
    const u64 size = page_count * PAGE_BYTES;
    if (!copy_zero_to_target_range(start, size)) {
        (void)unmap_reply_target_pages(start, page_count);
        return SYSCALL_ERR_MAP;
    }
    if (map_prot != prot) {
        const u64 protect_status = apply_target_pages(start, page_count, prot, protect_reply_target_pages);
        if (protect_status != SYSCALL_OK) {
            (void)unmap_reply_target_pages(start, page_count);
            return protect_status;
        }
    }
    return SYSCALL_OK;
}

static int has_thread_group_peers(void) {
    if (!g_proc || g_proc->pid == 0) return 0;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *peer = &g_processes[i];
        if (!peer->used || peer == g_proc || peer->exec_pending || peer->principal == 0 || peer->pid != g_proc->pid) continue;
        return 1;
    }
    return 0;
}

static int is_thread_group_peer(const struct linux_process_state *peer) {
    return g_proc && peer && peer->used && peer != g_proc && !peer->exec_pending &&
        peer->principal != 0 && peer->pid == g_proc->pid;
}

static void sync_current_vm_metadata_to_thread_group_peers(void) {
    if (!g_proc || g_proc->pid == 0) return;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *peer = &g_processes[i];
        if (!is_thread_group_peer(peer)) continue;
        peer->mmap_next_va = g_proc->mmap_next_va;
        peer->brk_next_va = g_proc->brk_next_va;
        for (u64 r = 0; r < VM_REGION_MAX; r++) peer->regions[r] = g_proc->regions[r];
    }
}

static int share_target_pages_to_thread_group_peers(u64 start, u64 page_count, u64 prot) {
    if (!g_proc || g_proc->pid == 0 || page_count == 0) return 1;
    const u64 kernel_prot = linux_prot_for_kernel_map(prot);
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *peer = &g_processes[i];
        if (!is_thread_group_peer(peer)) continue;
        const u64 status = share_reply_target_pages_to_trap_target(peer->principal, start, page_count, kernel_prot);
        if (status != SYSCALL_OK) {
            user_log("LinuxAbiServer: thread-group share pages failed peer=");
            user_log_hex_inline(peer->principal);
            user_log(" va=");
            user_log_hex_inline(start);
            user_log(" pages=");
            user_log_hex_inline(page_count);
            user_log(" status=");
            user_log_hex_value(status);
            return 0;
        }
    }
    return 1;
}

static int protect_target_pages_to_thread_group_peers(u64 start, u64 page_count, u64 prot) {
    if (!g_proc || g_proc->pid == 0 || page_count == 0) return 1;
    const u64 kernel_prot = linux_prot_for_kernel_map(prot);
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *peer = &g_processes[i];
        if (!is_thread_group_peer(peer)) continue;
        const u64 status = protect_trap_target_pages(peer->principal, start, page_count, kernel_prot);
        if (status != SYSCALL_OK) {
            user_log("LinuxAbiServer: thread-group protect pages failed peer=");
            user_log_hex_inline(peer->principal);
            user_log(" va=");
            user_log_hex_inline(start);
            user_log(" pages=");
            user_log_hex_inline(page_count);
            user_log(" status=");
            user_log_hex_value(status);
            return 0;
        }
    }
    return 1;
}

static int unmap_target_pages_from_thread_group_peers(u64 start, u64 page_count) {
    if (!g_proc || g_proc->pid == 0 || page_count == 0) return 1;
    for (u64 i = 0; i < LINUX_PROCESS_MAX; i++) {
        struct linux_process_state *peer = &g_processes[i];
        if (!is_thread_group_peer(peer)) continue;
        const u64 status = unmap_trap_target_pages(peer->principal, start, page_count);
        if (status != SYSCALL_OK) {
            user_log("LinuxAbiServer: thread-group unmap pages failed peer=");
            user_log_hex_inline(peer->principal);
            user_log(" va=");
            user_log_hex_inline(start);
            user_log(" pages=");
            user_log_hex_inline(page_count);
            user_log(" status=");
            user_log_hex_value(status);
            return 0;
        }
    }
    return 1;
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

static int vm_fault_trace_enabled(void) {
    return g_proc != 0 && g_proc->fault_trace_enabled != 0;
}

static void vm_fault_trace_context(const struct trap_request *req) {
    if (!vm_fault_trace_enabled()) return;
    user_log("LinuxAbiServer.fault principal=");
    user_log_hex_inline(req->caller_principal);
    user_log(" thread=");
    user_log_hex_inline(req->thread_id);
    user_log(" rip=");
    user_log_hex_inline(req->rip);
    user_log(" rsp=");
    user_log_hex_inline(req->rsp);
    user_log(" cr2=");
    user_log_hex_inline(req->fault_addr);
    user_log(" ec=");
    user_log_hex_inline(req->error_code);
    user_log(" rax=");
    user_log_hex_inline(req->rax);
    user_log(" rbx=");
    user_log_hex_inline(req->rbx);
    user_log(" rcx=");
    user_log_hex_inline(req->rcx);
    user_log(" rdx=");
    user_log_hex_inline(req->rdx);
    user_log(" rsi=");
    user_log_hex_inline(req->rsi);
    user_log(" rdi=");
    user_log_hex_inline(req->rdi);
    user_log(" r8=");
    user_log_hex_inline(req->r8);
    user_log(" r9=");
    user_log_hex_inline(req->r9);
    user_log(" r10=");
    user_log_hex_inline(req->r10);
    user_log(" r11=");
    user_log_hex_inline(req->r11);
    user_log(" r12=");
    user_log_hex_inline(req->r12);
    user_log(" r13=");
    user_log_hex_inline(req->r13);
    user_log(" r14=");
    user_log_hex_inline(req->r14);
    user_log(" r15=");
    user_log_hex_inline(req->r15);
    user_log(" fs=");
    user_log_hex_inline(req->fs_base);
    user_log(" gs=");
    user_log_hex_inline(req->gs_base);
    user_log("\n");
}

static void vm_fault_trace_decision(const char *decision, u64 a, u64 b, u64 c) {
    if (!vm_fault_trace_enabled()) return;
    user_log("LinuxAbiServer.fault decision=");
    user_log(decision);
    user_log(" a=");
    user_log_hex_inline(a);
    user_log(" b=");
    user_log_hex_inline(b);
    user_log(" c=");
    user_log_hex_inline(c);
    user_log("\n");
}

static int unmap_tracked_target_range(u64 start, u64 size) {
    vm_trace4("vm.unmap_tracked.begin", start, size, 0, 0);
    if (!vm_range_covered(start, size)) return 1;
    if (!vm_split_range_boundaries(start, size)) return 0;
    const u64 end = start + size;
    if (!unmap_target_pages_from_thread_group_peers(start, size / PAGE_BYTES)) return 0;
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used || g_regions[i].file_lazy) continue;
        const u64 rs = g_regions[i].start;
        const u64 re = rs + g_regions[i].size;
        if (rs < start || re > end) continue;
        u64 done = 0;
        const u64 page_count = g_regions[i].size / PAGE_BYTES;
        while (done < page_count) {
            const u64 chunk = min_u64(page_count - done, 64);
            const u64 va = rs + done * PAGE_BYTES;
            const u64 status = unmap_reply_target_pages(va, chunk);
            if (status != SYSCALL_OK) {
                vm_trace4("vm.unmap_tracked.current_fail", va, chunk, status, 0);
                return 0;
            }
            done += chunk;
        }
    }
    vm_remove_range(start, size);
    sync_current_vm_metadata_to_thread_group_peers();
    vm_trace4("vm.unmap_tracked.done", start, size, size / PAGE_BYTES, 0);
    return 1;
}

static void clear_tracked_target_ranges(void) {
    for (u64 i = 0; i < VM_REGION_MAX; i++) g_regions[i].used = 0;
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

static int copy_zero_to_target_range(u64 dst, u64 len) {
    u8 zeros[256];
    for (u64 i = 0; i < sizeof(zeros); i++) zeros[i] = 0;
    u64 done = 0;
    while (done < len) {
        u64 chunk = len - done;
        if (chunk > sizeof(zeros)) chunk = sizeof(zeros);
        if (copy_to_target(dst + done, zeros, chunk) != chunk) return 0;
        done += chunk;
    }
    return 1;
}

static int materialize_lazy_file_range(u64 start, u64 size, u64 prot) {
    if (!vm_split_range_boundaries(start, size)) {
        vm_trace4("vm.lazy.split_fail", start, size, prot, vm_region_used_count());
        return 0;
    }
    const u64 end = start + size;
    for (;;) {
        u64 slot = VM_REGION_MAX;
        for (u64 i = 0; i < VM_REGION_MAX; i++) {
            if (!g_regions[i].used || !g_regions[i].file_lazy) continue;
            const u64 rs = g_regions[i].start;
            const u64 re = rs + g_regions[i].size;
            if (rs >= start && re <= end) {
                slot = i;
                break;
            }
        }
        if (slot == VM_REGION_MAX) break;
        struct vm_region region = g_regions[slot];
        const u64 page_count = region.size / PAGE_BYTES;
        vm_trace4("vm.lazy.region", region.start, region.size, region.file_offset, region.file_size);
        const u64 map_prot = ((prot | 0x2) & ~0x4);
        const u64 map_status = map_target_pages_chunked(region.start, page_count, map_prot);
        if (map_status != SYSCALL_OK) {
            vm_trace4("vm.lazy.map_fail", region.start, page_count, map_prot, map_status);
            return 0;
        }

        struct fd_entry shadow_fd;
        memset(&shadow_fd, 0, sizeof(shadow_fd));
        shadow_fd.kind = FD_FILE;
        shadow_fd.token = region.file_token;
        shadow_fd.size = region.file_size;
        int fault = 0;
        const u64 expected = region.file_offset < region.file_size ? min_u64(region.size, region.file_size - region.file_offset) : 0;
        const u64 copied = read_fd_at_to_fresh_target_pages(&shadow_fd, region.file_offset, region.start, expected, &fault);
        if (fault || copied < expected) {
            vm_trace4("vm.lazy.read_fail", region.start, copied, expected, (u64)fault);
            (void)unmap_reply_target_pages(region.start, page_count);
            return 0;
        }
        if (copied < region.size && !copy_zero_to_target_range(region.start + copied, region.size - copied)) {
            vm_trace4("vm.lazy.zero_fail", region.start + copied, region.size - copied, region.start, region.size);
            (void)unmap_reply_target_pages(region.start, page_count);
            return 0;
        }
        if (map_prot != prot) {
            const u64 protect_status = apply_target_pages(region.start, page_count, prot, protect_reply_target_pages);
            if (protect_status != SYSCALL_OK) {
                vm_trace4("vm.lazy.protect_fail", region.start, page_count, prot, protect_status);
                (void)unmap_reply_target_pages(region.start, page_count);
                return 0;
            }
        }
        g_regions[slot].file_lazy = 0;
        g_regions[slot].prot = prot;
    }
    return 1;
}

static int materialize_file_vm_object_range(u64 start, u64 size, u64 prot) {
    if (!vm_split_range_boundaries(start, size)) {
        vm_trace4("vm.object.split_fail", start, size, prot, vm_region_used_count());
        return 0;
    }
    const u64 end = start + size;
    for (;;) {
        u64 slot = VM_REGION_MAX;
        for (u64 i = 0; i < VM_REGION_MAX; i++) {
            if (!g_regions[i].used || !g_regions[i].file_vm_object) continue;
            const u64 rs = g_regions[i].start;
            const u64 re = rs + g_regions[i].size;
            if (rs >= start && re <= end) {
                slot = i;
                break;
            }
        }
        if (slot == VM_REGION_MAX) break;
        struct vm_region region = g_regions[slot];
        const u64 page_count = region.size / PAGE_BYTES;
        const u64 map_prot = ((prot | 0x2) & ~0x4);
        u8 page[PAGE_BYTES];
        for (u64 page_index = 0; page_index < page_count; page_index++) {
            const u64 va = region.start + page_index * PAGE_BYTES;
            if (copy_from_target(va, page, PAGE_BYTES) != PAGE_BYTES) {
                vm_trace4("vm.object.copy_in_fail", va, PAGE_BYTES, region.start, region.size);
                return 0;
            }
            const u64 unmap_status = unmap_reply_target_pages(va, 1);
            if (unmap_status != SYSCALL_OK) {
                vm_trace4("vm.object.unmap_fail", va, 1, unmap_status, 0);
                return 0;
            }
            const u64 map_status = map_target_pages_chunked(va, 1, map_prot);
            if (map_status != SYSCALL_OK) {
                vm_trace4("vm.object.map_fail", va, 1, map_prot, map_status);
                return 0;
            }
            if (copy_to_target(va, page, PAGE_BYTES) != PAGE_BYTES) {
                vm_trace4("vm.object.copy_out_fail", va, PAGE_BYTES, region.start, region.size);
                (void)unmap_reply_target_pages(va, 1);
                return 0;
            }
            if (map_prot != prot) {
                const u64 protect_status = protect_reply_target_pages(va, 1, linux_prot_for_kernel_map(prot));
                if (protect_status != SYSCALL_OK) {
                    vm_trace4("vm.object.protect_fail", va, 1, prot, protect_status);
                    (void)unmap_reply_target_pages(va, 1);
                    return 0;
                }
            }
        }
        if (page_count == 0) {
            vm_trace4("vm.object.empty", region.start, region.size, start, size);
            return 0;
        }
        g_regions[slot].file_vm_object = 0;
        g_regions[slot].prot = prot;
    }
    return 1;
}

static int materialize_lazy_file_prefix_before(u64 end_va, u64 file_token, u64 file_offset) {
    u64 cursor_va = end_va;
    u64 cursor_offset = file_offset;
    for (;;) {
        u64 slot = VM_REGION_MAX;
        for (u64 i = 0; i < VM_REGION_MAX; i++) {
            if (!g_regions[i].used || !g_regions[i].file_backed || !g_regions[i].file_lazy) continue;
            if (g_regions[i].file_token != file_token) continue;
            const u64 rs = g_regions[i].start;
            const u64 re = rs + g_regions[i].size;
            if (re != cursor_va) continue;
            if (g_regions[i].file_offset + g_regions[i].size != cursor_offset) continue;
            slot = i;
            break;
        }
        if (slot == VM_REGION_MAX) return 1;
        const u64 start = g_regions[slot].start;
        const u64 size = g_regions[slot].size;
        const u64 offset = g_regions[slot].file_offset;
        const u64 prot = g_regions[slot].prot;
        if (!materialize_lazy_file_range(start, size, prot)) return 0;
        cursor_va = start;
        cursor_offset = offset;
    }
}

static struct ipc_message handle_page_fault(const struct trap_request *req) {
    enum { PF_PRESENT = 1 << 0, PF_WRITE = 1 << 1, PF_INSTRUCTION = 1 << 4 };
    const u64 fault_page = page_down(req->fault_addr);
    vm_fault_trace_context(req);
    vm_trace4("vm.fault.begin", req->fault_addr, req->error_code, req->rax, req->caller_principal);
    vm_trace4("vm.fault.context", req->rip, req->rsp, req->rdi, req->rsi);
    for (u64 i = 0; i < VM_REGION_MAX; i++) {
        if (!g_regions[i].used) continue;
        const u64 rs = g_regions[i].start;
        const u64 re = rs + g_regions[i].size;
        if (fault_page < rs || fault_page >= re) continue;
        const u64 prot = g_regions[i].prot;
        vm_fault_trace_decision("region", rs, re - rs, prot);
        vm_trace4("vm.fault.region", rs, re - rs, prot, ((u64)g_regions[i].file_lazy << 1) | (u64)g_regions[i].file_vm_object);
        if ((req->error_code & PF_WRITE) != 0 && (prot & 0x2) == 0) {
            vm_fault_trace_decision("deny_write", fault_page, prot, req->error_code);
            vm_trace4("vm.fault.deny_write", fault_page, prot, req->error_code, 0);
            return terminate_current_linux_process_from_trap(req->caller_principal, 139, 139, TRAP_RESPONSE_FLAG_EXIT);
        }
        if ((req->error_code & PF_INSTRUCTION) != 0 && (prot & 0x4) == 0) {
            vm_fault_trace_decision("deny_exec", fault_page, prot, req->error_code);
            vm_trace4("vm.fault.deny_exec", fault_page, prot, req->error_code, 0);
            return terminate_current_linux_process_from_trap(req->caller_principal, 139, 139, TRAP_RESPONSE_FLAG_EXIT);
        }
        if (g_regions[i].file_backed && g_regions[i].file_lazy) {
            u64 size = LINUX_FILE_FAULT_CLUSTER_PAGES * PAGE_BYTES;
            if (size > re - fault_page) size = re - fault_page;
            if (!materialize_lazy_file_range(fault_page, size, prot)) {
                vm_fault_trace_decision("lazy_fail", fault_page, size, prot);
                vm_trace4("vm.fault.lazy_fail", fault_page, size, prot, 0);
                return terminate_current_linux_process_from_trap(req->caller_principal, 139, 139, TRAP_RESPONSE_FLAG_EXIT);
            }
            vm_fault_trace_decision("lazy_ok", fault_page, size, prot);
            vm_trace4("vm.fault.lazy_ok", fault_page, size, prot, 0);
            return reply(req->rax, 0);
        }
        if (g_regions[i].file_vm_object && (req->error_code & PF_WRITE) != 0 && (prot & 0x2) != 0) {
            u64 size = LINUX_FILE_FAULT_CLUSTER_PAGES * PAGE_BYTES;
            if (size > re - fault_page) size = re - fault_page;
            if (!materialize_file_vm_object_range(fault_page, size, prot)) return terminate_current_linux_process_from_trap(req->caller_principal, 139, 139, TRAP_RESPONSE_FLAG_EXIT);
            return reply(req->rax, 0);
        }
        if ((req->error_code & PF_PRESENT) != 0) {
            const u64 status = protect_reply_target_pages(fault_page, 1, prot);
            if (status != SYSCALL_OK) {
                vm_fault_trace_decision("protect_fail", fault_page, prot, status);
                return terminate_current_linux_process_from_trap(req->caller_principal, 139, 139, TRAP_RESPONSE_FLAG_EXIT);
            }
            vm_fault_trace_decision("protect_ok", fault_page, prot, status);
            return reply(req->rax, 0);
        }
        vm_fault_trace_decision("unhandled", fault_page, prot, req->error_code);
        vm_trace4("vm.fault.unhandled", fault_page, prot, req->error_code, 0);
        return terminate_current_linux_process_from_trap(req->caller_principal, 139, 139, TRAP_RESPONSE_FLAG_EXIT);
    }
    vm_fault_trace_decision("no_region", req->fault_addr, req->error_code, req->rax);
    vm_trace4("vm.fault.no_region", req->fault_addr, req->error_code, req->rax, 0);
    return terminate_current_linux_process_from_trap(req->caller_principal, 139, 139, TRAP_RESPONSE_FLAG_EXIT);
}

static struct ipc_message handle_mmap(const struct trap_request *req) {
    enum { PROT_READ = 0x1, PROT_WRITE = 0x2, PROT_EXEC = 0x4, MAP_SHARED = 0x01, MAP_PRIVATE = 0x02, MAP_SHARED_VALIDATE = 0x03, MAP_TYPE = 0x0F, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20, MAP_32BIT = 0x40, MAP_FIXED_NOREPLACE = 0x100000 };
    const u64 requested_va = req->args[0]; const u64 len = req->args[1]; u64 prot = req->args[2] & (PROT_READ | PROT_WRITE | PROT_EXEC); const u64 flags = req->args[3]; const u64 fd = req->args[4]; const u64 offset = req->args[5]; const u64 map_type = flags & MAP_TYPE;
    const int file_backed = (flags & MAP_ANONYMOUS) == 0;
    if (len == 0) return reply(errno_inval(), 0); if (map_type != MAP_PRIVATE && map_type != MAP_SHARED && map_type != MAP_SHARED_VALIDATE) return reply(errno_inval(), 0); if ((offset & (PAGE_BYTES - 1)) != 0) return reply(errno_inval(), 0); if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0 && (requested_va == 0 || (requested_va & (PAGE_BYTES - 1)) != 0)) return reply(errno_inval(), 0); if (file_backed && (!fd_valid(fd) || g_fds[fd].kind != FD_FILE)) return reply(errno_badf(), 0); prot = normalize_linux_prot(prot);
    if (file_backed && (prot & PROT_WRITE) != 0 && (map_type == MAP_SHARED || map_type == MAP_SHARED_VALIDATE)) {
        return reply(errno_acces(), 0);
    }
    u64 size = 0;
    if (!page_up_checked(len, &size)) return reply(errno_nomem(), 0);
    const u64 page_count = size / PAGE_BYTES;
    const int fixed_mapping = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
    u64 target_va = fixed_mapping ? requested_va : (((flags & MAP_32BIT) != 0) ? find_mmap32_area(size) : find_mmap_area(size));
    if (!fixed_mapping && (flags & MAP_32BIT) == 0 && target_va != 0 && target_va < normalized_mmap_base()) {
        g_mmap_next_va = normalized_mmap_base();
        target_va = find_mmap_area(size);
    }
    const u64 map_prot = file_backed ? ((prot | PROT_WRITE) & ~PROT_EXEC) : prot;
    const int has_peers = has_thread_group_peers();
    int cache_vm_object_file = 0;
    if (LINUX_ENABLE_FILE_VM_OBJECT_MMAP && file_backed && map_type == MAP_PRIVATE) {
        g_prof.file_vm_object_mmap_considered++;
        if (!has_peers && (flags & MAP_FIXED) == 0 && g_fds[fd].path_len != 0 &&
            cacheable_readonly_path(g_fds[fd].path) &&
            g_fds[fd].size != 0 &&
            align_up(g_fds[fd].size, PAGE_BYTES) <= (u64)LINUX_FILE_VM_OBJECT_MAX_PAGES * PAGE_BYTES)
        {
            g_prof.file_vm_object_mmap_candidates++;
            cache_vm_object_file = 1;
        }
    }
    const int keep_private_file_writable = file_backed && map_type == MAP_PRIVATE && (prot & PROT_EXEC) == 0;
    const u64 effective_prot = cache_vm_object_file ? prot : (keep_private_file_writable ? map_prot : prot);
    const int lazy_file = !cache_vm_object_file && file_backed && map_type == MAP_PRIVATE && !has_peers && (flags & MAP_FIXED) == 0;
    vm_trace4("vm.mmap.begin", requested_va, len, prot, flags);
    if (target_va == 0 || !linux_user_range_valid(target_va, size)) return reply(errno_nomem(), 0);
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
    if ((flags & MAP_FIXED) != 0 && file_backed && map_type == MAP_PRIVATE &&
        vm_range_has_file_backing(target_va, size, g_fds[fd].token, offset))
    {
        if (!has_peers && g_fds[fd].path_len != 0 &&
            cacheable_readonly_path(g_fds[fd].path) &&
            g_fds[fd].size != 0 &&
            align_up(g_fds[fd].size, PAGE_BYTES) <= (u64)LINUX_FILE_VM_OBJECT_MAX_PAGES * PAGE_BYTES)
        {
            if (!unmap_tracked_target_range(target_va, size)) return reply(errno_nomem(), 0);
            if (file_vm_object_map_to_target(&g_fds[fd], offset, target_va, size, effective_prot)) {
                if (!vm_add_region(target_va, size, effective_prot, g_fds[fd].token, offset, g_fds[fd].size, 1, 0, 1)) {
                    (void)unmap_reply_target_pages(target_va, page_count);
                    return reply(errno_nomem(), 0);
                }
                if (!share_target_pages_to_thread_group_peers(target_va, page_count, effective_prot)) {
                    (void)unmap_tracked_target_range(target_va, size);
                    return reply(errno_nomem(), 0);
                }
                sync_current_vm_metadata_to_thread_group_peers();
                vm_trace4("vm.mmap.reuse_vm_object", target_va, size, effective_prot, flags);
                return reply(target_va, 0);
            }
            const u64 fallback_status = map_target_pages_chunked(target_va, page_count, map_prot);
            if (fallback_status != SYSCALL_OK) return reply(errno_nomem(), 0);
            int fault = 0;
            const u64 copied = read_fd_at_to_fresh_target_pages(&g_fds[fd], offset, target_va, len, &fault);
            if (fault) return reply(errno_fault(), 0);
            const u64 expected = offset < g_fds[fd].size ? min_u64(len, g_fds[fd].size - offset) : 0;
            if (copied < expected) return reply(errno_io(), 0);
            if (copied < size && !copy_zero_to_target_range(target_va + copied, size - copied)) {
                (void)unmap_reply_target_pages(target_va, page_count);
                return reply(errno_fault(), 0);
            }
            if (map_prot != effective_prot) {
                const u64 protect_status = apply_target_pages(target_va, page_count, effective_prot, protect_reply_target_pages);
                if (protect_status != SYSCALL_OK) {
                    (void)unmap_reply_target_pages(target_va, page_count);
                    return reply(errno_nomem(), 0);
                }
            }
            if (!vm_add_region(target_va, size, effective_prot, g_fds[fd].token, offset, g_fds[fd].size, 1, 0, 0)) {
                (void)unmap_reply_target_pages(target_va, page_count);
                return reply(errno_nomem(), 0);
            }
            if (!share_target_pages_to_thread_group_peers(target_va, page_count, effective_prot)) {
                (void)unmap_tracked_target_range(target_va, size);
                return reply(errno_nomem(), 0);
            }
            sync_current_vm_metadata_to_thread_group_peers();
            vm_trace4("vm.mmap.reuse_exec_fallback", target_va, size, effective_prot, flags);
            return reply(target_va, 0);
        }
        if (!vm_split_range_boundaries(target_va, size)) return reply(errno_nomem(), 0);
        if ((effective_prot & PROT_WRITE) != 0 &&
            !materialize_file_vm_object_range(target_va, size, effective_prot))
        {
            return reply(errno_nomem(), 0);
        }
        if (!(LINUX_ENABLE_FILE_PAGE_FAULT_LAZY && vm_range_has_lazy_file_backing(target_va, size))) {
            if (LINUX_MATERIALIZE_FILE_PREFIX_BEFORE_FIXED &&
                !materialize_lazy_file_prefix_before(target_va, g_fds[fd].token, offset))
            {
                return reply(errno_nomem(), 0);
            }
            if (!materialize_lazy_file_range(target_va, size, effective_prot)) return reply(errno_nomem(), 0);
        }
        if (!vm_apply_mapped_pages_in_range(target_va, size, effective_prot, protect_reply_target_pages)) return reply(errno_nomem(), 0);
        vm_protect_range(target_va, size, effective_prot);
        if (!protect_target_pages_to_thread_group_peers(target_va, page_count, effective_prot)) return reply(errno_nomem(), 0);
        sync_current_vm_metadata_to_thread_group_peers();
        vm_trace4("vm.mmap.reuse", target_va, size, effective_prot, flags);
        return reply(target_va, 0);
    }
    if ((flags & MAP_FIXED) != 0 && !unmap_overlapping_target_range(target_va, size)) return reply(errno_nomem(), 0);
    u64 map_status = (lazy_file || cache_vm_object_file) ? SYSCALL_OK :
        (file_backed ? map_target_pages_chunked(target_va, page_count, map_prot) :
            map_zeroed_target_pages_chunked(target_va, page_count, prot));
    if (map_status == SYSCALL_ERR_MAP && !fixed_mapping && (flags & MAP_32BIT) == 0) {
        g_mmap_next_va = target_va + size;
        target_va = find_mmap_area(size);
        if (target_va != 0 && linux_user_range_valid(target_va, size)) {
            map_status = (lazy_file || cache_vm_object_file) ? SYSCALL_OK :
                (file_backed ? map_target_pages_chunked(target_va, page_count, map_prot) :
                    map_zeroed_target_pages_chunked(target_va, page_count, prot));
        }
    }
    if (map_status == SYSCALL_OK) {
        int mapped_from_cache = 0;
        if (cache_vm_object_file) {
            mapped_from_cache = file_vm_object_map_to_target(&g_fds[fd], offset, target_va, size, prot);
            if (!mapped_from_cache) {
                g_prof.file_vm_object_mmap_fallbacks++;
                const u64 fallback_status = map_target_pages_chunked(target_va, page_count, map_prot);
                if (fallback_status != SYSCALL_OK) return reply(errno_nomem(), 0);
            }
        }
        if (file_backed && !lazy_file && !mapped_from_cache) {
            int fault = 0;
            const u64 copied = read_fd_at_to_fresh_target_pages(&g_fds[fd], offset, target_va, len, &fault);
            if (fault) return reply(errno_fault(), 0);
            const u64 expected = offset < g_fds[fd].size ? min_u64(len, g_fds[fd].size - offset) : 0;
            const int trace_file_mmap = profile_trace_enabled() && g_proc && g_proc->profile_verbose_enabled && g_fds[fd].path_len != 0;
            if (trace_file_mmap || copied < expected) {
                user_log("LinuxAbiServer.trace mmap.file path=");
                user_log(g_fds[fd].path_len != 0 ? g_fds[fd].path : "(unknown)");
                user_log(" off=");
                user_log_hex_inline(offset);
                user_log(" len=");
                user_log_hex_inline(len);
                user_log(" size=");
                user_log_hex_inline(g_fds[fd].size);
                user_log(" copied=");
                user_log_hex_inline(copied);
                user_log(" expected=");
                user_log_hex_inline(expected);
                user_log("\n");
            }
            if (copied < expected) return reply(errno_io(), 0);
            if (copied < size && !copy_zero_to_target_range(target_va + copied, size - copied)) {
                (void)unmap_reply_target_pages(target_va, page_count);
                return reply(errno_fault(), 0);
            }
            if (map_prot != effective_prot) {
                const u64 protect_status = apply_target_pages(target_va, page_count, effective_prot, protect_reply_target_pages);
                if (protect_status != SYSCALL_OK) {
                    user_log("LinuxAbiServer: mmap file-backed protect failed\n");
                    user_log_hex_value(protect_status);
                    return reply(errno_nomem(), 0);
                }
            }
        }
        if (!vm_add_region(target_va, size, effective_prot, file_backed ? g_fds[fd].token : 0, file_backed ? offset : 0, file_backed ? g_fds[fd].size : 0, file_backed, lazy_file, mapped_from_cache)) {
            user_log("LinuxAbiServer: mmap vm_add_region failed\n");
            user_log_hex_value(target_va);
            user_log_hex_value(size);
            user_log("LinuxAbiServer: vm regions used=");
            user_log_dec_value(vm_region_used_count());
            user_log("\n");
            if (!lazy_file) (void)unmap_reply_target_pages(target_va, page_count);
            return reply(errno_nomem(), 0);
        }
        if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) == 0) g_mmap_next_va = target_va + size;
        if (!share_target_pages_to_thread_group_peers(target_va, page_count, effective_prot)) {
            (void)unmap_tracked_target_range(target_va, size);
            return reply(errno_nomem(), 0);
        }
        sync_current_vm_metadata_to_thread_group_peers();
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
    u64 old_size = 0;
    u64 new_size = 0;
    if (!page_up_checked(old_len, &old_size) || !page_up_checked(new_len, &new_size)) return reply(errno_nomem(), 0);
    if (!linux_user_range_valid(old_addr, old_size)) return reply(errno_nomem(), 0);
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
    if (!linux_user_range_valid(old_addr, new_size)) return reply(errno_nomem(), 0);
    if ((flags & MREMAP_FIXED) == 0 && vm_range_uncovered(grow_addr, extra_size)) {
        const u64 extra_pages = extra_size / PAGE_BYTES;
        const u64 status = map_zeroed_target_pages_chunked(grow_addr, extra_pages, prot);
        if (status == SYSCALL_OK) {
            if (!vm_add_region(grow_addr, extra_size, prot, 0, 0, 0, 0, 0, 0)) {
                (void)unmap_tracked_target_range(grow_addr, extra_size);
                return reply(errno_nomem(), 0);
            }
            if (!share_target_pages_to_thread_group_peers(grow_addr, extra_pages, prot)) {
                (void)unmap_tracked_target_range(grow_addr, extra_size);
                return reply(errno_nomem(), 0);
            }
            sync_current_vm_metadata_to_thread_group_peers();
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
    if (req->args[0] < g_brk_initial_va || req->args[0] > brk_limit_exclusive()) return reply(g_brk_next_va, 0);
    if (req->args[0] > g_brk_next_va) {
        u64 from = 0;
        u64 to = 0;
        if (!page_up_checked(g_brk_next_va, &from) || !page_up_checked(req->args[0], &to)) return reply(g_brk_next_va, 0);
        if (to > from) {
            if (!linux_user_range_valid(from, to - from)) return reply(g_brk_next_va, 0);
            const u64 status = map_zeroed_target_pages_chunked(from, (to - from) / PAGE_BYTES, 0x3);
            if (status != SYSCALL_OK) return reply(g_brk_next_va, 0);
            if (!vm_add_region(from, to - from, 0x3, 0, 0, 0, 0, 0, 0)) {
                user_log("LinuxAbiServer: brk vm_add_region failed\n");
                user_log_hex_value(from);
                user_log_hex_value(to - from);
                return reply(g_brk_next_va, 0);
            }
            if (!share_target_pages_to_thread_group_peers(from, (to - from) / PAGE_BYTES, 0x3)) {
                (void)unmap_tracked_target_range(from, to - from);
                return reply(g_brk_next_va, 0);
            }
        }
    }
    g_brk_next_va = req->args[0];
    sync_current_vm_metadata_to_thread_group_peers();
    return reply(g_brk_next_va, 0);
}

static struct ipc_message handle_mprotect(const struct trap_request *req) {
    const u64 start = req->args[0]; const u64 len = req->args[1]; const u64 prot = normalize_linux_prot(req->args[2]);
    if (len == 0) return reply(0, 0);
    if ((start & (PAGE_BYTES - 1)) != 0) return reply(errno_inval(), 0);
    u64 size = 0;
    if (!page_up_checked(len, &size) || !linux_user_range_valid(start, size)) return reply(errno_nomem(), 0);
    vm_trace4("vm.mprotect.begin", start, size, prot, 0);
    g_prof.mprotect_calls++;
    g_prof.mprotect_pages += size / PAGE_BYTES;
    const int tracked = vm_range_covered(start, size);
    if (tracked && !vm_split_range_boundaries(start, size)) return reply(errno_nomem(), 0);
    if (tracked && (prot & 0x2) != 0 && !materialize_file_vm_object_range(start, size, prot)) return reply(errno_nomem(), 0);
    const int protect_ok = tracked ?
        vm_apply_mapped_pages_in_range(start, size, prot, protect_reply_target_pages) :
        (apply_target_pages(start, size / PAGE_BYTES, prot, protect_reply_target_pages) == SYSCALL_OK);
    if (!tracked && !protect_ok) return reply(0, 0);
    if (!protect_ok) {
        user_log("LinuxAbiServer: mprotect current target failed\n");
        user_log_hex_value(start);
        user_log_hex_value(size / PAGE_BYTES);
        user_log_hex_value(prot);
        return reply(errno_nomem(), 0);
    }
    if (tracked) vm_protect_range(start, size, prot);
    if (tracked && !protect_target_pages_to_thread_group_peers(start, size / PAGE_BYTES, prot)) return reply(errno_nomem(), 0);
    if (tracked) sync_current_vm_metadata_to_thread_group_peers();
    vm_trace4("vm.mprotect.done", start, size, prot, 0);
    return reply(0, 0);
}

static struct ipc_message handle_mincore(const struct trap_request *req) {
    const u64 start = req->args[0];
    const u64 len = req->args[1];
    const u64 vec = req->args[2];
    if (len == 0) return reply(errno_inval(), 0);
    if (vec == 0 || (start & (PAGE_BYTES - 1)) != 0) return reply(errno_inval(), 0);
    u64 size = 0;
    if (!page_up_checked(len, &size) || !linux_user_range_valid(start, size)) return reply(errno_nomem(), 0);
    const u64 page_count = size / PAGE_BYTES;
    if (!vm_range_covered(start, size)) return reply(errno_nomem(), 0);

    u8 resident[64];
    for (u64 i = 0; i < sizeof(resident); i++) resident[i] = 1;
    u64 done = 0;
    while (done < page_count) {
        u64 chunk = page_count - done;
        if (chunk > sizeof(resident)) chunk = sizeof(resident);
        if (copy_to_target(vec + done, resident, chunk) != chunk) return reply(errno_fault(), 0);
        done += chunk;
    }
    return reply(0, 0);
}

static struct ipc_message handle_munmap(const struct trap_request *req) {
    const u64 start = req->args[0]; const u64 len = req->args[1];
    if (len == 0) return reply(errno_inval(), 0);
    if ((start & (PAGE_BYTES - 1)) != 0) return reply(errno_inval(), 0);
    u64 size = 0;
    if (!page_up_checked(len, &size) || !linux_user_range_valid(start, size)) return reply(errno_inval(), 0);
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
