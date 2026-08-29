#include "../lpr_filed_internal.h"

typedef struct lpr_cpuid_result {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
} lpr_cpuid_result_t;

typedef struct lpr_cpuid_flag {
    uint32_t mask;
    char name[24];
} lpr_cpuid_flag_t;

enum {
    /* The complete supported flag vocabulary plus every field at its maximum
     * decimal width is 746 bytes.  Keep one bounded record with explicit
     * headroom rather than issuing dozens of partial snapshot writes. */
    LPR_PROC_CPU_RECORD_BYTES = 1024u,
};

typedef struct lpr_proc_buffer {
    char bytes[LPR_PROC_CPU_RECORD_BYTES];
    uint64_t length;
} lpr_proc_buffer_t;

static lpr_cpuid_result_t lpr_cpuid(uint32_t leaf, uint32_t subleaf)
{
    lpr_cpuid_result_t result;
    __asm__ volatile(
        "cpuid"
        : "=a"(result.eax), "=b"(result.ebx),
          "=c"(result.ecx), "=d"(result.edx)
        : "a"(leaf), "c"(subleaf));
    return result;
}

static int64_t lpr_proc_write_all(uint64_t fd, const void *buffer, uint64_t length)
{
    const uint8_t *bytes = (const uint8_t *)buffer;
    uint64_t written = 0;
    while (written < length) {
        const int64_t status = lpr_linux_write(
            fd,
            (uint64_t)(uintptr_t)(bytes + written),
            length - written);
        if (status < 0) return status;
        if (status == 0) return -LPR_LINUX_EIO;
        written += (uint64_t)status;
    }
    return 0;
}

static int64_t lpr_proc_write_string(uint64_t fd, const char *string)
{
    return lpr_proc_write_all(fd, string, (uint64_t)lpr_strlen(string));
}

static uint64_t lpr_proc_format_u64(char *buffer, uint64_t value)
{
    char reverse[24];
    uint64_t count = 0;
    do {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0 && count < sizeof(reverse));
    for (uint64_t i = 0; i < count; ++i) {
        buffer[i] = reverse[count - i - 1u];
    }
    return count;
}

static int64_t lpr_proc_buffer_append(
    lpr_proc_buffer_t *buffer,
    const void *data,
    uint64_t length)
{
    if (buffer == 0 || (data == 0 && length != 0)) {
        return -LPR_LINUX_EFAULT;
    }
    if (length > sizeof(buffer->bytes) - buffer->length) {
        return -LPR_LINUX_EOVERFLOW;
    }
    if (length != 0) {
        lpr_memcpy(buffer->bytes + buffer->length, data, (size_t)length);
        buffer->length += length;
    }
    return 0;
}

static int64_t lpr_proc_buffer_append_string(
    lpr_proc_buffer_t *buffer,
    const char *string)
{
    if (string == 0) return -LPR_LINUX_EFAULT;
    return lpr_proc_buffer_append(buffer, string, (uint64_t)lpr_strlen(string));
}

static int64_t lpr_proc_buffer_append_u64(
    lpr_proc_buffer_t *buffer,
    uint64_t value)
{
    char digits[24];
    const uint64_t length = lpr_proc_format_u64(digits, value);
    return lpr_proc_buffer_append(buffer, digits, length);
}

static int64_t lpr_proc_buffer_append_field_u64(
    lpr_proc_buffer_t *buffer,
    const char *name,
    uint64_t value,
    const char *suffix)
{
    int64_t status = lpr_proc_buffer_append_string(buffer, name);
    if (status == 0) status = lpr_proc_buffer_append_u64(buffer, value);
    if (status == 0) status = lpr_proc_buffer_append_string(buffer, suffix);
    return status;
}

static int64_t lpr_proc_write_u64(uint64_t fd, uint64_t value)
{
    char buffer[24];
    const uint64_t length = lpr_proc_format_u64(buffer, value);
    return lpr_proc_write_all(fd, buffer, length);
}

static int64_t lpr_proc_write_field_u64(
    uint64_t fd,
    const char *name,
    uint64_t value,
    const char *suffix)
{
    int64_t status = lpr_proc_write_string(fd, name);
    if (status == 0) status = lpr_proc_write_u64(fd, value);
    if (status == 0) status = lpr_proc_write_string(fd, suffix);
    return status;
}

static int64_t lpr_proc_snapshot_create(const char *name, uint64_t flags)
{
    const uint64_t known_flags =
        LPR_LINUX_O_ACCMODE |
        LPR_LINUX_O_NONBLOCK |
        LPR_LINUX_O_LARGEFILE |
        LPR_LINUX_O_CLOEXEC |
        LPR_LINUX_O_NOFOLLOW;
    if ((flags & ~known_flags) != 0) return -LPR_LINUX_EINVAL;
    if ((flags & LPR_LINUX_O_ACCMODE) != LPR_LINUX_O_RDONLY) {
        return -LPR_LINUX_EACCES;
    }
    return lpr_linux_memfd_create(
        (uint64_t)(uintptr_t)name,
        LPR_LINUX_MFD_ALLOW_SEALING);
}

static int64_t lpr_proc_snapshot_finish(
    uint64_t staging_fd,
    const char *path,
    uint64_t flags)
{
    int64_t status = lpr_linux_lseek(staging_fd, 0, 0);
    if (status < 0) {
        (void)lpr_linux_close(staging_fd);
        return status;
    }
    status = lpr_linux_fcntl(
        staging_fd,
        LPR_LINUX_F_ADD_SEALS,
        LPR_LINUX_F_SEAL_SEAL |
            LPR_LINUX_F_SEAL_SHRINK |
            LPR_LINUX_F_SEAL_GROW |
            LPR_LINUX_F_SEAL_WRITE);
    if (status != 0) {
        (void)lpr_linux_close(staging_fd);
        return status;
    }

    const lpr_filed_backend_t *staging = lpr_filed_backend(staging_fd);
    if (staging == 0) {
        (void)lpr_linux_close(staging_fd);
        return -LPR_LINUX_EIO;
    }
    const uint8_t seal_state = staging->reserved1;
    uint64_t handle = 0;
    status = lpr_filed_dup_handle(
        staging->handle,
        (flags & LPR_LINUX_O_CLOEXEC) != 0 ? FILED_FD_CLOEXEC : 0,
        &handle);
    if (status != 0) {
        (void)lpr_linux_close(staging_fd);
        return status;
    }
    const int fd = lpr_fd_alloc(handle, flags);
    if (fd < 0) {
        (void)lpr_filed_close_handle(handle);
        (void)lpr_linux_close(staging_fd);
        return fd;
    }
    lpr_filed_backend_t *snapshot = lpr_filed_backend((uint64_t)(uint32_t)fd);
    if (snapshot == 0) {
        (void)lpr_linux_close((uint64_t)(uint32_t)fd);
        (void)lpr_linux_close(staging_fd);
        return -LPR_LINUX_EIO;
    }
    snapshot->reserved1 = seal_state;
    const size_t path_length = lpr_strnlen(path, sizeof(snapshot->open_path));
    if (path_length < sizeof(snapshot->open_path)) {
        lpr_memcpy(snapshot->open_path, path, path_length + 1u);
    }
    (void)lpr_linux_close(staging_fd);
    return fd;
}

static void lpr_cpuid_vendor(char out[13], const lpr_cpuid_result_t *leaf0)
{
    lpr_memcpy(out + 0, &leaf0->ebx, 4);
    lpr_memcpy(out + 4, &leaf0->edx, 4);
    lpr_memcpy(out + 8, &leaf0->ecx, 4);
    out[12] = '\0';
}

static void lpr_cpuid_brand(char out[49])
{
    lpr_memset(out, 0, 49);
    const lpr_cpuid_result_t max_extended = lpr_cpuid(0x80000000u, 0);
    if (max_extended.eax < 0x80000004u) {
        lpr_memcpy(out, "Unknown x86_64 CPU", 19);
        return;
    }
    for (uint32_t leaf = 0; leaf < 3; ++leaf) {
        const lpr_cpuid_result_t part = lpr_cpuid(0x80000002u + leaf, 0);
        lpr_memcpy(out + leaf * 16u + 0u, &part.eax, 4);
        lpr_memcpy(out + leaf * 16u + 4u, &part.ebx, 4);
        lpr_memcpy(out + leaf * 16u + 8u, &part.ecx, 4);
        lpr_memcpy(out + leaf * 16u + 12u, &part.edx, 4);
    }
    uint64_t start = 0;
    while (start < 48u && out[start] == ' ') ++start;
    uint64_t end = 48;
    while (end > start && (out[end - 1u] == ' ' || out[end - 1u] == '\0')) --end;
    if (start != 0 && end > start) {
        lpr_memmove(out, out + start, (size_t)(end - start));
    }
    out[end - start] = '\0';
}

static int64_t lpr_proc_buffer_append_flag_word(
    lpr_proc_buffer_t *buffer,
    uint32_t word,
    const lpr_cpuid_flag_t *flags,
    uint64_t flag_count)
{
    for (uint64_t i = 0; i < flag_count; ++i) {
        if ((word & flags[i].mask) == 0) continue;
        int64_t status = lpr_proc_buffer_append_string(buffer, " ");
        if (status == 0) {
            status = lpr_proc_buffer_append_string(buffer, flags[i].name);
        }
        if (status != 0) return status;
    }
    return 0;
}

static int64_t lpr_proc_buffer_append_cpu_flags(
    lpr_proc_buffer_t *buffer,
    const lpr_cpuid_result_t *leaf1,
    const lpr_cpuid_result_t *leaf7,
    const lpr_cpuid_result_t *extended1)
{
    static const lpr_cpuid_flag_t leaf1_edx[] = {
        { 1u << 0, "fpu" }, { 1u << 1, "vme" }, { 1u << 2, "de" },
        { 1u << 3, "pse" }, { 1u << 4, "tsc" }, { 1u << 5, "msr" },
        { 1u << 6, "pae" }, { 1u << 7, "mce" }, { 1u << 8, "cx8" },
        { 1u << 9, "apic" }, { 1u << 11, "sep" }, { 1u << 12, "mtrr" },
        { 1u << 13, "pge" }, { 1u << 14, "mca" }, { 1u << 15, "cmov" },
        { 1u << 16, "pat" }, { 1u << 17, "pse36" }, { 1u << 19, "clflush" },
        { 1u << 23, "mmx" }, { 1u << 24, "fxsr" }, { 1u << 25, "sse" },
        { 1u << 26, "sse2" }, { 1u << 28, "ht" },
    };
    static const lpr_cpuid_flag_t leaf1_ecx[] = {
        { 1u << 0, "pni" }, { 1u << 1, "pclmulqdq" },
        { 1u << 3, "monitor" }, { 1u << 5, "vmx" }, { 1u << 6, "smx" },
        { 1u << 7, "est" }, { 1u << 9, "ssse3" }, { 1u << 12, "fma" },
        { 1u << 13, "cx16" }, { 1u << 17, "pcid" },
        { 1u << 19, "sse4_1" }, { 1u << 20, "sse4_2" },
        { 1u << 21, "x2apic" }, { 1u << 22, "movbe" },
        { 1u << 23, "popcnt" }, { 1u << 24, "tsc_deadline_timer" },
        { 1u << 25, "aes" }, { 1u << 26, "xsave" },
        { 1u << 27, "osxsave" }, { 1u << 28, "avx" },
        { 1u << 29, "f16c" }, { 1u << 30, "rdrand" },
        { 1u << 31, "hypervisor" },
    };
    static const lpr_cpuid_flag_t leaf7_ebx[] = {
        { 1u << 0, "fsgsbase" }, { 1u << 3, "bmi1" },
        { 1u << 5, "avx2" }, { 1u << 7, "smep" }, { 1u << 8, "bmi2" },
        { 1u << 9, "erms" }, { 1u << 10, "invpcid" },
        { 1u << 16, "avx512f" }, { 1u << 17, "avx512dq" },
        { 1u << 18, "rdseed" }, { 1u << 19, "adx" },
        { 1u << 20, "smap" }, { 1u << 23, "clflushopt" },
        { 1u << 24, "clwb" }, { 1u << 29, "sha_ni" },
        { 1u << 30, "avx512bw" }, { 1u << 31, "avx512vl" },
    };
    static const lpr_cpuid_flag_t extended_edx[] = {
        { 1u << 11, "syscall" }, { 1u << 20, "nx" },
        { 1u << 26, "pdpe1gb" }, { 1u << 27, "rdtscp" },
        { 1u << 29, "lm" },
    };
    static const lpr_cpuid_flag_t extended_ecx[] = {
        { 1u << 0, "lahf_lm" }, { 1u << 2, "svm" },
        { 1u << 5, "abm" }, { 1u << 6, "sse4a" },
        { 1u << 8, "3dnowprefetch" }, { 1u << 11, "xop" },
        { 1u << 16, "fma4" }, { 1u << 21, "tbm" },
        { 1u << 22, "topoext" },
    };

    int64_t status = lpr_proc_buffer_append_string(buffer, "flags\t\t:");
    if (status == 0) status = lpr_proc_buffer_append_flag_word(
        buffer, leaf1->edx, leaf1_edx, sizeof(leaf1_edx) / sizeof(leaf1_edx[0]));
    if (status == 0) status = lpr_proc_buffer_append_flag_word(
        buffer, leaf1->ecx, leaf1_ecx, sizeof(leaf1_ecx) / sizeof(leaf1_ecx[0]));
    if (status == 0) status = lpr_proc_buffer_append_flag_word(
        buffer, leaf7->ebx, leaf7_ebx, sizeof(leaf7_ebx) / sizeof(leaf7_ebx[0]));
    if (status == 0) status = lpr_proc_buffer_append_flag_word(
        buffer, extended1->edx, extended_edx,
        sizeof(extended_edx) / sizeof(extended_edx[0]));
    if (status == 0) status = lpr_proc_buffer_append_flag_word(
        buffer, extended1->ecx, extended_ecx,
        sizeof(extended_ecx) / sizeof(extended_ecx[0]));
    if (status == 0) status = lpr_proc_buffer_append_string(buffer, "\n\n");
    return status;
}

static int64_t lpr_proc_write_cpuinfo(uint64_t fd)
{
    pachaos_system_info_t system_info;
    lpr_memset(&system_info, 0, sizeof(system_info));
    int64_t status = lpr_pacha_syscall1(
        PACHAOS_SYSCALL_SYSTEM_INFO,
        (uint64_t)(uintptr_t)&system_info);
    if (status != 0) {
        return lpr_pacha_status_to_errno(status);
    }
    const lpr_cpuid_result_t leaf0 = lpr_cpuid(0, 0);
    lpr_cpuid_result_t leaf1;
    lpr_cpuid_result_t leaf7;
    lpr_cpuid_result_t extended1;
    lpr_memset(&leaf1, 0, sizeof(leaf1));
    lpr_memset(&leaf7, 0, sizeof(leaf7));
    lpr_memset(&extended1, 0, sizeof(extended1));
    if (leaf0.eax >= 1u) leaf1 = lpr_cpuid(1, 0);
    if (leaf0.eax >= 7u) leaf7 = lpr_cpuid(7, 0);
    const lpr_cpuid_result_t max_extended = lpr_cpuid(0x80000000u, 0);
    if (max_extended.eax >= 0x80000001u) {
        extended1 = lpr_cpuid(0x80000001u, 0);
    }
    char vendor[13];
    char brand[49];
    lpr_cpuid_vendor(vendor, &leaf0);
    lpr_cpuid_brand(brand);
    uint64_t family = (leaf1.eax >> 8u) & 0x0fu;
    uint64_t model = (leaf1.eax >> 4u) & 0x0fu;
    const uint64_t stepping = leaf1.eax & 0x0fu;
    const uint64_t extended_family = (leaf1.eax >> 20u) & 0xffu;
    const uint64_t extended_model = (leaf1.eax >> 16u) & 0x0fu;
    if (family == 0x0fu) family += extended_family;
    if (family == 0x06u || family == 0x0fu) model += extended_model << 4u;

    uint64_t cpu_count = system_info.online_cpu_count;
    if (cpu_count == 0) cpu_count = 1;
    for (uint64_t cpu = 0; cpu < cpu_count; ++cpu) {
        lpr_proc_buffer_t record;
        lpr_memset(&record, 0, sizeof(record));
        status = lpr_proc_buffer_append_field_u64(
            &record, "processor\t: ", cpu, "\n");
        if (status == 0) status = lpr_proc_buffer_append_string(&record, "vendor_id\t: ");
        if (status == 0) status = lpr_proc_buffer_append_string(&record, vendor);
        if (status == 0) status = lpr_proc_buffer_append_string(&record, "\n");
        if (status == 0) status = lpr_proc_buffer_append_field_u64(
            &record, "cpu family\t: ", family, "\n");
        if (status == 0) status = lpr_proc_buffer_append_field_u64(
            &record, "model\t\t: ", model, "\n");
        if (status == 0) status = lpr_proc_buffer_append_string(&record, "model name\t: ");
        if (status == 0) status = lpr_proc_buffer_append_string(&record, brand);
        if (status == 0) status = lpr_proc_buffer_append_string(&record, "\n");
        if (status == 0) status = lpr_proc_buffer_append_field_u64(
            &record, "stepping\t: ", stepping, "\n");
        if (status == 0) status = lpr_proc_buffer_append_field_u64(
            &record, "siblings\t: ", cpu_count, "\n");
        if (status == 0) status = lpr_proc_buffer_append_field_u64(
            &record, "cpu cores\t: ", cpu_count, "\n");
        if (status == 0) status = lpr_proc_buffer_append_cpu_flags(
            &record, &leaf1, &leaf7, &extended1);
        if (status != 0) return status;
        status = lpr_proc_write_all(fd, record.bytes, record.length);
        if (status != 0) return status;
    }
    return 0;
}

static int64_t lpr_proc_write_meminfo(uint64_t fd)
{
    pachaos_system_info_t system_info;
    lpr_memset(&system_info, 0, sizeof(system_info));
    const int64_t raw_status = lpr_pacha_syscall1(
        PACHAOS_SYSCALL_SYSTEM_INFO,
        (uint64_t)(uintptr_t)&system_info);
    if (raw_status != 0) return lpr_pacha_status_to_errno(raw_status);
    uint64_t total_kb = system_info.total_usable_memory_bytes / 1024u;
    uint64_t free_kb = system_info.free_memory_bytes / 1024u;
    if (free_kb > total_kb) free_kb = total_kb;
    int64_t status = lpr_proc_write_field_u64(fd, "MemTotal:       ", total_kb, " kB\n");
    if (status == 0) status = lpr_proc_write_field_u64(fd, "MemFree:        ", free_kb, " kB\n");
    if (status == 0) status = lpr_proc_write_field_u64(fd, "MemAvailable:   ", free_kb, " kB\n");
    if (status == 0) status = lpr_proc_write_string(
        fd,
        "Buffers:               0 kB\n"
        "Cached:                0 kB\n"
        "SwapCached:            0 kB\n"
        "Active:                0 kB\n"
        "Inactive:              0 kB\n"
        "Shmem:                 0 kB\n"
        "SwapTotal:             0 kB\n"
        "SwapFree:              0 kB\n");
    return status;
}

static int64_t lpr_proc_write_mounts(uint64_t fd)
{
    return lpr_proc_write_string(
        fd,
        "rootfs / ext4 rw,relatime 0 0\n"
        "tmpfs /run tmpfs rw,nosuid,nodev 0 0\n"
        "tmpfs /dev/shm tmpfs rw,nosuid,nodev 0 0\n"
        "proc /proc proc ro,nosuid,nodev,noexec,relatime 0 0\n");
}

typedef struct lpr_proc_root_entry {
    const char *name;
    uint64_t mode;
} lpr_proc_root_entry_t;

static int lpr_proc_emit_dirent(
    uint8_t *out,
    uint64_t capacity,
    uint64_t *written,
    const char *name,
    uint64_t inode,
    uint64_t mode,
    uint64_t next_offset)
{
    const uint64_t name_length = (uint64_t)lpr_strlen(name);
    const uint16_t record_length = lpr_dirent_reclen(name_length);
    if (*written > capacity || record_length > capacity - *written) return 0;

    uint8_t *record = out + *written;
    lpr_memset(record, 0, record_length);
    *(uint64_t *)(void *)(record + 0u) = inode;
    *(int64_t *)(void *)(record + 8u) = next_offset > 0x7fffffffffffffffull ?
        (int64_t)0x7fffffffffffffffull : (int64_t)next_offset;
    *(uint16_t *)(void *)(record + 16u) = record_length;
    *(uint8_t *)(void *)(record + 18u) = lpr_dtype_from_mode(mode);
    lpr_memcpy(record + 19u, name, (size_t)name_length);
    record[19u + name_length] = '\0';
    *written += record_length;
    return 1;
}

int64_t lpr_linux_proc_getdents64(uint64_t fd, uint64_t buf, uint64_t count)
{
    static const lpr_proc_root_entry_t fixed_entries[] = {
        { ".", LPR_LINUX_S_IFDIR },
        { "..", LPR_LINUX_S_IFDIR },
        { "self", LPR_LINUX_S_IFLNK },
        { "cpuinfo", LPR_LINUX_S_IFREG },
        { "meminfo", LPR_LINUX_S_IFREG },
        { "mounts", LPR_LINUX_S_IFREG },
        { "sys", LPR_LINUX_S_IFDIR },
        { "overflowuid", LPR_LINUX_S_IFREG },
        { "overflowgid", LPR_LINUX_S_IFREG },
    };
    enum { LPR_PROC_PID_BATCH = 128u };

    const lpr_filed_backend_t *file = lpr_filed_backend(fd);
    if (file == 0 ||
        (lpr_strcmp(file->open_path, "/proc") != 0 &&
         lpr_strcmp(file->open_path, "/proc/") != 0))
    {
        return -LPR_LINUX_ENOENT;
    }
    if (buf == 0 && count != 0) return -LPR_LINUX_EFAULT;
    if (count == 0) return 0;

    uint8_t *out = (uint8_t *)(uintptr_t)buf;
    uint64_t written = 0;
    uint64_t offset = lpr_filed_control_offset(fd);
    const uint64_t fixed_count =
        sizeof(fixed_entries) / sizeof(fixed_entries[0]);

    while (offset < fixed_count) {
        const lpr_proc_root_entry_t *entry = &fixed_entries[offset];
        if (!lpr_proc_emit_dirent(
                out,
                count,
                &written,
                entry->name,
                offset + 1u,
                entry->mode,
                offset + 1u))
        {
            if (written == 0) return -LPR_LINUX_EINVAL;
            lpr_filed_control_set_offset(fd, offset);
            return (int64_t)written;
        }
        offset += 1u;
    }

    uint64_t pids[LPR_PROC_PID_BATCH];
    uint64_t pid_count = 0;
    const int list_status = lpr_supervisor_list_processes(
        offset - fixed_count,
        pids,
        LPR_PROC_PID_BATCH,
        &pid_count);
    if (list_status != 0) {
        lpr_filed_control_set_offset(fd, offset);
        return written != 0 ? (int64_t)written : list_status;
    }

    for (uint64_t i = 0; i < pid_count; ++i) {
        char name[24];
        const uint64_t name_length = lpr_proc_format_u64(name, pids[i]);
        name[name_length] = '\0';
        if (!lpr_proc_emit_dirent(
                out,
                count,
                &written,
                name,
                0x7000000000000000ull + pids[i],
                LPR_LINUX_S_IFDIR,
                offset + 1u))
        {
            if (written == 0) return -LPR_LINUX_EINVAL;
            break;
        }
        offset += 1u;
    }
    lpr_filed_control_set_offset(fd, offset);
    return (int64_t)written;
}

int64_t lpr_linux_proc_snapshot_open(const char *path, uint64_t flags)
{
    enum {
        LPR_PROC_NONE = 0,
        LPR_PROC_CPUINFO,
        LPR_PROC_MEMINFO,
        LPR_PROC_MOUNTS,
    } kind = LPR_PROC_NONE;
    const char *name = 0;
    if (path == 0) return -LPR_LINUX_EFAULT;
    if (lpr_strcmp(path, "/proc/cpuinfo") == 0) {
        kind = LPR_PROC_CPUINFO;
        name = "proc-cpuinfo";
    } else if (lpr_strcmp(path, "/proc/meminfo") == 0) {
        kind = LPR_PROC_MEMINFO;
        name = "proc-meminfo";
    } else if (lpr_strcmp(path, "/proc/mounts") == 0) {
        kind = LPR_PROC_MOUNTS;
        name = "proc-mounts";
    } else {
        return -LPR_LINUX_ENOENT;
    }

    const int64_t staging_fd = lpr_proc_snapshot_create(name, flags);
    if (staging_fd < 0) return staging_fd;
    int64_t status = 0;
    switch (kind) {
    case LPR_PROC_CPUINFO:
        status = lpr_proc_write_cpuinfo((uint64_t)staging_fd);
        break;
    case LPR_PROC_MEMINFO:
        status = lpr_proc_write_meminfo((uint64_t)staging_fd);
        break;
    case LPR_PROC_MOUNTS:
        status = lpr_proc_write_mounts((uint64_t)staging_fd);
        break;
    default:
        status = -LPR_LINUX_ENOENT;
        break;
    }
    if (status != 0) {
        (void)lpr_linux_close((uint64_t)staging_fd);
        return status;
    }
    return lpr_proc_snapshot_finish((uint64_t)staging_fd, path, flags);
}
