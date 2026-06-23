#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pacha/capsule.h"

#define DEMO_PAGE_SIZE 4096ULL
#define DEMO_DMA_IOVA 0x100000000ULL

static int is_fd(int fd) {
    return fd >= 16 && fd < 256;
}

static void print_info(const struct pacha_capsule_info *info) {
    printf("fd=%" PRIu64 "\n", info->fd);
    printf("kind=%" PRIu64 " rights=0x%016" PRIx64 "\n",
        info->kind,
        info->rights);
    printf("owner=%" PRIu64 "\n", info->owner);
    printf("device=0x%016" PRIx64 " object=%" PRIu64 " user-va=0x%016" PRIx64 "\n",
        info->device,
        info->object_id,
        info->user_va);
    printf("iova=0x%016" PRIx64 " size=%" PRIu64 " index=%" PRIu64 " flags=0x%016" PRIx64 "\n",
        info->iova,
        info->size,
        info->index,
        info->flags);
}

static int query_expect(int fd, uint64_t kind, const char *label) {
    struct pacha_capsule_info info;
    memset(&info, 0, sizeof(info));
    const int status = pacha_capsule_expect_kind(fd, kind, &info);
    if (status != 0) {
        printf("%s-query-failed status=%d\n", label, status);
        return 0;
    }
    printf("%s-query-ok\n", label);
    print_info(&info);
    return 1;
}

static int require_fd_kind(int fd, uint64_t kind, const char *label) {
    if (!is_fd(fd)) {
        printf("%s-fd-bad fd=%d expected-kind=%" PRIu64 "\n", label, fd, kind);
        return 0;
    }
    return query_expect(fd, kind, label);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "env-query") == 0) {
        const char *env_fd = getenv("KOBOX_PACHAOS_DEVICE_FD");
        if (env_fd == NULL || env_fd[0] == '\0') {
            puts("capsule-env-missing");
            return 3;
        }
        argv[1] = (char *)env_fd;
        argc = 3;
        argv[2] = "query";
    }

    if (argc == 1) {
        struct pacha_capsule_info info;
        const int status = pacha_capsule_query(0, &info);
        if (status < 0) {
            puts("pacha-native-syscall-ok");
            puts("device-fd-invalid-ok");
            return 0;
        }
        printf("device-fd-invalid-unexpected status=%d\n", status);
        return 1;
    }

    char *end = NULL;
    errno = 0;
    const long parsed = strtol(argv[1], &end, 0);
    if (errno != 0 || end == argv[1] || *end != '\0' || parsed < 0 || parsed > 255) {
        fprintf(stderr, "invalid device fd: %s\n", argv[1]);
        return 64;
    }
    const int device_fd = (int)parsed;

    struct pacha_capsule_info info;
    const int status = pacha_capsule_expect_kind(device_fd, PACHA_CAPSULE_KIND_DEVICE, &info);
    if (status != 0) {
        printf("device-query-failed status=%d\n", status);
        return 1;
    }
    puts("device-query-ok");
    print_info(&info);

    if (argc > 2 && strcmp(argv[2], "query") == 0) return 0;

    const int mmio_fd = pacha_capsule_derive_mmio(device_fd, 0, NULL, DEMO_PAGE_SIZE, 0);
    if (!require_fd_kind(mmio_fd, PACHA_CAPSULE_KIND_MMIO, "mmio")) return 1;
    puts("mmio-derive-ok");

    void *dma_page = mmap(NULL, DEMO_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (dma_page == MAP_FAILED) {
        perror("mmap dma page");
        return 1;
    }
    memset(dma_page, 0xA5, DEMO_PAGE_SIZE);

    const int dma_buffer_fd = pacha_capsule_derive_dma_buffer(device_fd, dma_page, DEMO_DMA_IOVA, DEMO_PAGE_SIZE, 0);
    if (!require_fd_kind(dma_buffer_fd, PACHA_CAPSULE_KIND_DMA_BUFFER, "dma-buffer")) return 1;
    puts("dma-buffer-derive-ok");

    const int dma_mapping_fd = pacha_capsule_derive_dma_mapping_from_buffer(
        dma_buffer_fd,
        DEMO_DMA_IOVA,
        DEMO_PAGE_SIZE,
        PACHA_CAPSULE_DMA_BIDIRECTIONAL,
        0);
    if (!require_fd_kind(dma_mapping_fd, PACHA_CAPSULE_KIND_DMA_MAPPING, "dma-mapping")) return 1;
    puts("dma-mapping-derive-ok");

    puts("capsule-demo-complete");
    return 0;
}
