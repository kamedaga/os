#include "pacha/capsule.h"
#include "pacha/syscall.h"

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned char u8;

enum {
    SMOKE_SYSCALL_LOG = 1,
    PROCESS_STANDARD_CONFIG_TARGET_VA = 0x3C002000,
    INIT_BOOTSTRAP_MAGIC = 0x49425453,
    INIT_BOOTSTRAP_VERSION = 18,
    INIT_CONFIG_MAGIC = 0x49425443,
    INIT_CONFIG_VERSION = 1,
    INIT_DEVICE_FLAG_PRESENT = 1,
    INIT_MAX_DEVICE_DESCRIPTORS = 8,
    INIT_MAX_DEVICE_QUEUE_GRANTS = 4,
};

struct device_queue_grant {
    u64 queue_index;
    u64 submit_token;
    u64 notify_token;
};

struct device_descriptor {
    u64 transport;
    u64 flags;
    u64 bootstrap_source_va;
    u64 vendor_id;
    u64 device_id;
    u64 subsystem_id;
    u64 pci_bus;
    u64 pci_device;
    u64 pci_function;
    u64 resource_id;
    u64 queue_count;
    u64 common_page_paddr;
    u64 notify_page_paddr;
    u64 isr_page_paddr;
    u64 device_page_paddr;
    u64 common_page_offset;
    u64 notify_page_offset;
    u64 isr_page_offset;
    u64 device_page_offset;
    u64 notify_off_multiplier;
    u64 init_iommu_token;
    u64 init_queue_grant_count;
    struct device_queue_grant init_queue_grants[INIT_MAX_DEVICE_QUEUE_GRANTS];
    u64 init_command_token;
    u64 init_device_fd;
};

struct boot_archive_descriptor {
    u64 flags;
    u64 image_va;
    u64 size_bytes;
    u64 page_count;
};

struct display_descriptor {
    u64 flags;
    u64 width;
    u64 height;
    u64 pitch;
    u64 framebuffer_paddr;
    u64 framebuffer_size_bytes;
};

struct spawn_page_descriptor {
    u64 kind;
    u64 subject;
    u64 flags;
    u64 source_va;
    u64 target_va;
    u64 spawn_flags;
};

struct init_config_page {
    u64 magic;
    u64 version;
    u64 descriptor_page_va;
    u64 reserved0;
};

struct init_descriptor_page {
    u64 magic;
    u64 version;
    u64 spawn_page_count;
    u64 device_count;
    u64 boot_image_count;
    u64 reserved_counts[3];
    struct boot_archive_descriptor bootfs_archive;
    struct display_descriptor primary_display;
    struct spawn_page_descriptor spawn_pages[8];
    struct device_descriptor devices[INIT_MAX_DEVICE_DESCRIPTORS];
};

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
}

static u64 smoke_syscall2(u64 nr, u64 a0, u64 a1) {
    return (u64)pacha_syscall2(nr, a0, a1);
}

static void log_text(const char *s) {
    (void)smoke_syscall2(SMOKE_SYSCALL_LOG, (u64)s, cstr_len(s));
}

static void log_hex(const char *prefix, u64 value) {
    static const char digits[] = "0123456789abcdef";
    char buf[96];
    u64 n = 0;
    while (prefix[n] != 0 && n + 19 < sizeof(buf)) {
        buf[n] = prefix[n];
        n++;
    }
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[n++] = digits[(value >> ((u64)i * 4)) & 0xf];
    }
    buf[n++] = '\n';
    (void)smoke_syscall2(SMOKE_SYSCALL_LOG, (u64)buf, n);
}

static int is_fd(u64 value) {
    return value >= 16 && value < 256;
}

static const struct device_descriptor *find_boot_device_fd(const struct init_descriptor_page *desc) {
    u64 count = desc->device_count;
    if (count > INIT_MAX_DEVICE_DESCRIPTORS) count = INIT_MAX_DEVICE_DESCRIPTORS;
    for (u64 i = 0; i < count; i++) {
        const struct device_descriptor *device = &desc->devices[i];
        if ((device->flags & INIT_DEVICE_FLAG_PRESENT) == 0) continue;
        if (is_fd(device->init_device_fd)) return device;
    }
    return 0;
}

void device_fd_boot_smoke_main(void) {
    log_text("[device_fd_boot_smoke] start\n");

    const struct init_config_page *cfg = (const struct init_config_page *)PROCESS_STANDARD_CONFIG_TARGET_VA;
    if (cfg->magic != INIT_CONFIG_MAGIC || cfg->version != INIT_CONFIG_VERSION || cfg->descriptor_page_va == 0) {
        log_hex("[device_fd_boot_smoke] config magic=", cfg->magic);
        log_hex("[device_fd_boot_smoke] config version=", cfg->version);
        return;
    }

    const struct init_descriptor_page *desc = (const struct init_descriptor_page *)cfg->descriptor_page_va;
    if (desc->magic != INIT_BOOTSTRAP_MAGIC || desc->version != INIT_BOOTSTRAP_VERSION) {
        log_hex("[device_fd_boot_smoke] descriptor magic=", desc->magic);
        log_hex("[device_fd_boot_smoke] descriptor version=", desc->version);
        return;
    }

    const struct device_descriptor *device = find_boot_device_fd(desc);
    if (!device) {
        log_hex("[device_fd_boot_smoke] device_count=", desc->device_count);
        return;
    }

    const int fd = (int)device->init_device_fd;
    struct pacha_capsule_info info = {0};
    const int status = pacha_capsule_expect_kind(fd, PACHA_CAPSULE_KIND_DEVICE, &info);
    if (status != 0) {
        log_hex("[device_fd_boot_smoke] query status=", (u64)(long long)status);
        log_hex("[device_fd_boot_smoke] fd=", (u64)(unsigned)fd);
        return;
    }
    if (info.fd != (u64)(unsigned)fd || info.kind != PACHA_CAPSULE_KIND_DEVICE || info.rights == 0) {
        log_hex("[device_fd_boot_smoke] info fd=", info.fd);
        log_hex("[device_fd_boot_smoke] info kind=", info.kind);
        log_hex("[device_fd_boot_smoke] info rights=", info.rights);
        return;
    }
    if (info.device == 0 || info.device != device->resource_id) {
        log_hex("[device_fd_boot_smoke] info device=", info.device);
        log_hex("[device_fd_boot_smoke] descriptor resource=", device->resource_id);
        return;
    }
    if (info.index < 0x50 || info.index > 0xc0 ||
        ((info.index - 0x50) % 16) != 0) {
        log_hex("[device_fd_boot_smoke] irq delivery base=", info.index);
        return;
    }

    struct pacha_capsule_irq irq = {0};
    const int irq_status =
        pacha_capsule_device_derive_irq(fd, PACHA_CAPSULE_IRQ_AUTO, 0, 0, &irq);
    if (irq_status != 0) {
        log_hex("[device_fd_boot_smoke] irq status=", (u64)(long long)irq_status);
        return;
    }
    if (!is_fd((u64)(unsigned)irq.fd) || irq.count != 0) {
        log_hex("[device_fd_boot_smoke] irq fd=", (u64)(unsigned)irq.fd);
        log_hex("[device_fd_boot_smoke] irq count=", irq.count);
        return;
    }
    (void)pacha_capsule_close(irq.fd);

    log_text("[device_fd_boot_smoke] OK\n");
}
