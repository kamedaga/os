/* Included by lpr_signal.c: all native operations here are one-shot. In
 * particular do NOT use the pager-retry wrapper: it dereferences the missing
 * user page, which is unsafe while a synchronous fault is already pending. */
static int64_t lpr_fault_native(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    register uint64_t r10 __asm__("r10") = 0;
    register uint64_t r8 __asm__("r8") = 0;
    register uint64_t r9 __asm__("r9") = 0;
    __asm__ volatile("syscall" : "+a"(nr), "+D"(a0), "+S"(a1), "+d"(a2),
        "+r"(r10), "+r"(r8), "+r"(r9) : : "rcx", "r11", "memory");
    return (int64_t)nr;
}

enum { LPR_FAULT_MAPS = 256 };
static struct lpr_fault_map {
    uint64_t start, length, offset;
    char path[192];
    unsigned published, invalid;
} lpr_fault_maps[LPR_FAULT_MAPS];
static unsigned lpr_fault_map_count, lpr_fault_map_writers;
static uint64_t lpr_fault_map_epoch;
static char lpr_fault_comm[32];
static unsigned lpr_fault_comm_state;

void lpr_fault_comm_record(const char *comm)
{
    unsigned expected = 0;
    if (!comm || !*comm || !__atomic_compare_exchange_n(&lpr_fault_comm_state,
        &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    unsigned i = 0;
    for (; i < sizeof(lpr_fault_comm) - 1 && comm[i]; ++i) lpr_fault_comm[i] = comm[i];
    lpr_fault_comm[i] = 0;
    __atomic_store_n(&lpr_fault_comm_state, 2, __ATOMIC_RELEASE);
}

uint64_t lpr_fault_map_begin(uint64_t start, uint64_t length)
{
    __atomic_add_fetch(&lpr_fault_map_writers, 1, __ATOMIC_SEQ_CST);
    uint64_t epoch = __atomic_add_fetch(&lpr_fault_map_epoch, 1, __ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < LPR_FAULT_MAPS; ++i) {
        struct lpr_fault_map *m = &lpr_fault_maps[i];
        if (!__atomic_load_n(&m->published, __ATOMIC_ACQUIRE)) continue;
        if (length && (start > UINT64_MAX - length ||
            (m->start < start + length && start < m->start + m->length)))
            __atomic_store_n(&m->invalid, 1, __ATOMIC_RELEASE);
    }
    return epoch;
}
void lpr_fault_map_end(void)
{
    __atomic_add_fetch(&lpr_fault_map_epoch, 1, __ATOMIC_SEQ_CST);
    __atomic_sub_fetch(&lpr_fault_map_writers, 1, __ATOMIC_SEQ_CST);
}
void lpr_fault_map_record(uint64_t start, uint64_t length, uint64_t offset, uint64_t handle, uint64_t epoch)
{
    if (!length || length > UINT64_MAX - start) return;
    /* Executable file mappings only; the records never recycle, so close,
     * munmap and concurrent faults cannot reclaim a reader's filename. */
    unsigned slot = __atomic_load_n(&lpr_fault_map_count, __ATOMIC_RELAXED);
    do {
        if (slot >= LPR_FAULT_MAPS) return;
    } while (!__atomic_compare_exchange_n(&lpr_fault_map_count, &slot, slot + 1,
        0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    struct lpr_fault_map *m = &lpr_fault_maps[slot];
    int64_t n = lpr_filed_handle_path(handle, m->path, sizeof(m->path) - 1, 0);
    if (n <= 0 || (uint64_t)n >= sizeof(m->path)) return;
    m->path[n] = 0;
    m->start = start; m->length = length; m->offset = offset;
    /* Overlapping concurrent VM operations make this record untrustworthy.
     * Losing a name is preferable to assigning an old file to a reused VA. */
    if (__atomic_load_n(&lpr_fault_map_writers, __ATOMIC_SEQ_CST) != 1 ||
        __atomic_load_n(&lpr_fault_map_epoch, __ATOMIC_SEQ_CST) != epoch) return;
    __atomic_store_n(&m->published, 1, __ATOMIC_RELEASE);
}

/* Native VM_QUERY proves range/read permission but not residency. WRITEV's
 * kernel usercopy validates/copies the source under its own VM protection;
 * absent pager pages return to us without the unsafe userspace retry read.
 * A fresh nonblocking private pipe gives us a bounded safe-copy round trip.
 * The 16-byte prefix keeps successful lengths outside native error codes
 * 1..6 even when RIP is the last byte of its page. No user load touches RIP. */
static unsigned lpr_fault_instruction_bytes(uint64_t rip, unsigned char bytes[16])
{
    pacha_vm_mapping_info_t vm;
    if (lpr_fault_native(PACHAOS_SYSCALL_VM_QUERY, rip, (uintptr_t)&vm, sizeof(vm)) ||
        !(vm.prot & PACHAOS_PROT_READ) || vm.start > rip || rip - vm.start >= vm.size)
        return 0;
    unsigned n = (unsigned)lpr_fault_byte_limit(rip);
    if (vm.size - (rip - vm.start) < n) n = (unsigned)(vm.size - (rip - vm.start));
    uint64_t pair[2] = {0, 0};
    if (lpr_fault_native(PACHAOS_SYSCALL_PIPE_CREATE, (uintptr_t)pair,
        PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC | PACHA_FD_FLAG_NONBLOCK, 0)) return 0;
    unsigned char scratch[32] = {0};
    struct { uint64_t base, length; } iov[2] = {
        {(uintptr_t)scratch, 16}, {rip, n}};
    unsigned copied = 0;
    if (lpr_fault_native(PACHAOS_SYSCALL_FD_WRITEV, pair[1], (uintptr_t)iov, 2) == 16 + n &&
        lpr_fault_native(PACHAOS_SYSCALL_FD_READ, pair[0], (uintptr_t)scratch, 16 + n) == 16 + n) {
        for (unsigned i = 0; i < n; ++i) bytes[i] = scratch[16 + i];
        copied = n;
    }
    (void)lpr_fault_native(PACHAOS_SYSCALL_FD_CLOSE, pair[0], 0, 0);
    (void)lpr_fault_native(PACHAOS_SYSCALL_FD_CLOSE, pair[1], 0, 0);
    return copied;
}

static void lpr_fault_emit(uint32_t sig, const lpr_pacha_trap_frame_t *context,
    int async, uint64_t vector, uint64_t error, uint64_t address)
{
    struct lpr_fault_diag d = {0};
    d.sig = sig; d.async = async; d.vector = vector; d.error = error; d.address = address;
    d.rip = context->rip; d.rsp = context->rsp;
    d.native_pid = (uint64_t)lpr_fault_native(PACHAOS_SYSCALL_GETPID, 0, 0, 0);
    d.native_tid = (uint64_t)lpr_fault_native(PACHAOS_SYSCALL_GETTID, 0, 0, 0);
    /* Linux TIDs are native TIDs in this runtime; process PIDs are supervised. */
    d.linux_tid = d.native_tid;
    d.linux_pid = lpr_linux_current_pid > 0 ? (uint64_t)lpr_linux_current_pid :
        lpr_process_manifest.linux_pid;
    if (__atomic_load_n(&lpr_fault_comm_state, __ATOMIC_ACQUIRE) == 2) d.comm = lpr_fault_comm;
    pacha_vm_mapping_info_t vm;
    if (!lpr_fault_native(PACHAOS_SYSCALL_VM_QUERY, d.rip, (uintptr_t)&vm, sizeof(vm)) &&
        vm.start <= d.rip && d.rip - vm.start < vm.size &&
        vm.offset <= UINT64_MAX - (d.rip - vm.start)) {
        d.mapped = 1; d.map_start = vm.start; d.map_offset = vm.offset + d.rip - vm.start;
        uint64_t epoch = __atomic_load_n(&lpr_fault_map_epoch, __ATOMIC_SEQ_CST);
        if (!__atomic_load_n(&lpr_fault_map_writers, __ATOMIC_SEQ_CST)) {
            for (unsigned i = 0; i < LPR_FAULT_MAPS; ++i) {
                const struct lpr_fault_map *m = &lpr_fault_maps[i];
                if (__atomic_load_n(&m->published, __ATOMIC_ACQUIRE) &&
                    !__atomic_load_n(&m->invalid, __ATOMIC_ACQUIRE) &&
                    m->start <= d.rip && d.rip - m->start < m->length &&
                    m->offset <= UINT64_MAX - (d.rip - m->start)) {
                    d.file = m->path; d.map_offset = m->offset + d.rip - m->start;
                    break;
                }
            }
            if (__atomic_load_n(&lpr_fault_map_writers, __ATOMIC_SEQ_CST) ||
                __atomic_load_n(&lpr_fault_map_epoch, __ATOMIC_SEQ_CST) != epoch) {
                d.file = 0;
                d.map_offset = vm.offset + d.rip - vm.start;
            }
        }
    }
    if (!async && vector == 6) d.byte_count = lpr_fault_instruction_bytes(d.rip, d.bytes);
    char line[768];
    size_t n = lpr_fault_format(line, sizeof(line), &d);
    /* Raw Linux write implementation: no libc/stdio, no retry on error.
     * Native LOG additionally reaches serial/boot capture, not kernel ring. */
    unsigned wait_not_ready = 0;
    (void)lpr_fault_native(PACHAOS_SYSCALL_LOG, (uintptr_t)line, n, 0);
    (void)lpr_linux_write(2, (uintptr_t)line, n, &wait_not_ready);
}
