#include "kobox/device_pachaos_capsule.h"
#include "kobox/module.h"
#include "kobox/shim.h"
#include "linux_subsystem/block/block.h"
#include "pacha/capsule.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    KOBOXD_BOOTSTRAP_MAGIC = 0x3150474b42584f4bull,
    KOBOXD_BOOTSTRAP_VERSION = 1,
    KOBOXD_BOOTSTRAP_VA = 0x3f000000ull,
    KOBOXD_BOOTSTRAP_MAX_MODULES = 8,
    KOBOXD_BOOTSTRAP_NAME_BYTES = 64,
};

struct koboxd_bootstrap {
    uint64_t magic;
    uint64_t version;
    uint64_t device_fd;
    uint64_t module_count;
    uint64_t modules_va;
};

struct koboxd_bootstrap_module {
    char name[KOBOXD_BOOTSTRAP_NAME_BYTES];
    uint64_t image_va;
    uint64_t image_size;
};

static const char *const koboxd_expected_modules[] = {
    "nvme-auth.ko",
    "nvme-core.ko",
    "nvme.ko",
    "crc16.ko",
    "mbcache.ko",
    "jbd2.ko",
    "ext4.ko",
};

static const char *status_name(kb_status_t status)
{
    switch (status) {
    case KB_OK: return "KB_OK";
    case KB_ERR_INVALID: return "KB_ERR_INVALID";
    case KB_ERR_NOT_FOUND: return "KB_ERR_NOT_FOUND";
    case KB_ERR_DENIED: return "KB_ERR_DENIED";
    case KB_ERR_NOMEM: return "KB_ERR_NOMEM";
    case KB_ERR_IO: return "KB_ERR_IO";
    case KB_ERR_UNSUPPORTED: return "KB_ERR_UNSUPPORTED";
    case KB_ERR_PCI_CONFIG: return "KB_ERR_PCI_CONFIG";
    default: return "KB_ERR_UNKNOWN";
    }
}

static int validate_bootstrap_package(const struct koboxd_bootstrap **out_bootstrap)
{
    const struct koboxd_bootstrap *bootstrap =
        (const struct koboxd_bootstrap *)(uintptr_t)KOBOXD_BOOTSTRAP_VA;
    if (out_bootstrap != NULL) {
        *out_bootstrap = NULL;
    }
    const uint64_t expected_count =
        sizeof(koboxd_expected_modules) / sizeof(koboxd_expected_modules[0]);
    if (bootstrap->magic != KOBOXD_BOOTSTRAP_MAGIC ||
        bootstrap->version != KOBOXD_BOOTSTRAP_VERSION ||
        bootstrap->device_fd < 16 ||
        bootstrap->module_count != expected_count ||
        bootstrap->module_count > KOBOXD_BOOTSTRAP_MAX_MODULES ||
        bootstrap->modules_va == 0)
    {
        fprintf(stderr,
            "[koboxd] bootstrap invalid magic=0x%llx version=%llu device_fd=%llu modules=%llu table=0x%llx\n",
            (unsigned long long)bootstrap->magic,
            (unsigned long long)bootstrap->version,
            (unsigned long long)bootstrap->device_fd,
            (unsigned long long)bootstrap->module_count,
            (unsigned long long)bootstrap->modules_va);
        return -1;
    }

    const struct koboxd_bootstrap_module *modules =
        (const struct koboxd_bootstrap_module *)(uintptr_t)bootstrap->modules_va;
    for (uint64_t i = 0; i < bootstrap->module_count; i++) {
        const struct koboxd_bootstrap_module *module = &modules[i];
        if (strncmp(module->name, koboxd_expected_modules[i], KOBOXD_BOOTSTRAP_NAME_BYTES) != 0 ||
            module->image_va == 0 ||
            module->image_size < 4)
        {
            fprintf(stderr,
                "[koboxd] bootstrap module invalid index=%llu name=%s va=0x%llx size=%llu\n",
                (unsigned long long)i,
                module->name,
                (unsigned long long)module->image_va,
                (unsigned long long)module->image_size);
            return -2;
        }

        const unsigned char *image = (const unsigned char *)(uintptr_t)module->image_va;
        if (image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
            fprintf(stderr, "[koboxd] bootstrap module is not ELF name=%s\n", module->name);
            return -3;
        }
        printf("[koboxd] bootstrap module=%s va=0x%llx bytes=%llu\n",
            module->name,
            (unsigned long long)module->image_va,
            (unsigned long long)module->image_size);
    }

    printf("[koboxd] bootstrap package OK\n");
    if (out_bootstrap != NULL) {
        *out_bootstrap = bootstrap;
    }
    return 0;
}

static const struct koboxd_bootstrap_module *find_module(
    const struct koboxd_bootstrap *bootstrap,
    const char *name)
{
    if (bootstrap == NULL || name == NULL) {
        return NULL;
    }
    const struct koboxd_bootstrap_module *modules =
        (const struct koboxd_bootstrap_module *)(uintptr_t)bootstrap->modules_va;
    for (uint64_t i = 0; i < bootstrap->module_count; i++) {
        if (strncmp(modules[i].name, name, KOBOXD_BOOTSTRAP_NAME_BYTES) == 0) {
            return &modules[i];
        }
    }
    return NULL;
}

static int load_one_module(
    const struct koboxd_bootstrap *bootstrap,
    kb_device_backend_t *backend,
    const char *name,
    int allow_missing_init)
{
    const struct koboxd_bootstrap_module *desc = find_module(bootstrap, name);
    if (desc == NULL || desc->image_va == 0 || desc->image_size == 0) {
        fprintf(stderr, "[koboxd] module missing name=%s\n", name);
        return -1;
    }

    kb_module_t *module = NULL;
    const kb_module_image_t image = {
        .data = (const void *)(uintptr_t)desc->image_va,
        .size = (size_t)desc->image_size,
        .name = desc->name,
    };
    kb_status_t status = kb_module_open_image(&image, backend, &module);
    if (status != KB_OK || module == NULL) {
        fprintf(stderr, "[koboxd] %s open failed status=%s(%d)\n",
            name,
            status_name(status),
            status);
        return -2;
    }

    int init_result = 0;
    printf("[koboxd] %s init begin\n", name);
    fflush(stdout);
    status = kb_module_call_init(module, &init_result);
    if (status == KB_ERR_NOT_FOUND && allow_missing_init) {
        printf("[koboxd] %s has no init_module\n", name);
        return 0;
    }
    printf("[koboxd] %s init returned status=%s(%d) result=%d\n",
        name,
        status_name(status),
        status,
        init_result);
    if (status != KB_OK || init_result != 0) {
        return -3;
    }
    return 0;
}

static void *wait_for_first_disk(void)
{
    for (unsigned i = 0; i < 2048; i++) {
        kb_run_deferred_work();
        (void)kb_handle_any_irq(1000000ull);
        void *disk = kb_block_subsystem_first_registered_disk();
        if (disk != NULL) {
            return disk;
        }
    }
    return NULL;
}

static int run_nvme_smoke(const struct koboxd_bootstrap *bootstrap)
{
    if (bootstrap == NULL) {
        return -1;
    }

    printf("[koboxd] NVMe start device_fd=%llu loader=%s\n",
        (unsigned long long)bootstrap->device_fd,
        kb_module_loader_version());

    kb_device_backend_t *backend = NULL;
    kb_status_t status = kb_pachaos_capsule_device_create(bootstrap->device_fd, &backend);
    if (status != KB_OK || backend == NULL) {
        fprintf(stderr, "[koboxd] device backend create failed status=%s(%d)\n",
            status_name(status),
            status);
        return -2;
    }

    for (unsigned bar_index = 0; bar_index < 2; bar_index++) {
        struct pacha_capsule_bar_info bar = {0};
        int bar_status = pacha_capsule_pci_bar_info((int)bootstrap->device_fd, bar_index, &bar);
        printf("[koboxd] pci bar%u status=%d start=0x%llx end=0x%llx size=0x%llx flags=0x%llx\n",
            bar_index,
            bar_status,
            (unsigned long long)bar.start,
            (unsigned long long)bar.end,
            (unsigned long long)bar.size,
            (unsigned long long)bar.flags);
    }

    kb_shim_set_device_backend(backend);
    int load_status = load_one_module(bootstrap, backend, "nvme-auth.ko", 1);
    if (load_status != 0) {
        return load_status;
    }
    load_status = load_one_module(bootstrap, backend, "nvme-core.ko", 1);
    if (load_status != 0) {
        return load_status;
    }
    load_status = load_one_module(bootstrap, backend, "nvme.ko", 0);
    if (load_status != 0) {
        return load_status;
    }
    printf("[koboxd] NVMe modules loaded\n");
    kb_shim_set_device_backend(backend);

    void *disk = wait_for_first_disk();
    if (disk == NULL) {
        fprintf(stderr, "[koboxd] NVMe module stack registered no disk\n");
        return -3;
    }

    unsigned char sector[512];
    memset(sector, 0, sizeof(sector));
    const int read_status = kb_block_subsystem_disk_read(disk, 0, sector, sizeof(sector));
    if (read_status != 0) {
        fprintf(stderr, "[koboxd] NVMe disk read sector0 failed status=%d\n", read_status);
        return -4;
    }
    printf("[koboxd] NVMe disk read sector0=%02x %02x %02x %02x OK\n",
        sector[0],
        sector[1],
        sector[2],
        sector[3]);
    return 0;
}

int main(int argc, char **argv)
{
    printf("[koboxd] start argc=%d argv0=%s\n",
        argc,
        (argc > 0 && argv != NULL && argv[0] != NULL) ? argv[0] : "(null)");

    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        printf("[koboxd] monotonic=%llu.%09llu\n",
            (unsigned long long)ts.tv_sec,
            (unsigned long long)ts.tv_nsec);
    } else {
        fprintf(stderr, "[koboxd] clock_gettime failed\n");
        return 2;
    }

    char *buf = malloc(64);
    if (buf == NULL) {
        fprintf(stderr, "[koboxd] malloc failed\n");
        return 3;
    }
    snprintf(buf, 64, "[koboxd] malloc/stdout OK\n");
    fputs(buf, stdout);
    free(buf);

    fprintf(stderr, "[koboxd] stderr OK\n");
    const struct koboxd_bootstrap *bootstrap = NULL;
    if (validate_bootstrap_package(&bootstrap) != 0) {
        return 4;
    }
    if (run_nvme_smoke(bootstrap) != 0) {
        return 5;
    }
    printf("[koboxd] ready\n");
    fflush(stdout);
    fflush(stderr);
    return 0;
}
