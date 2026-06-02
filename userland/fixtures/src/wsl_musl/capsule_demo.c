#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pacha_capsule_abi.h"
#include "pacha_syscall_abi.h"

#define PACHA_SYSCALL_ERR_INVALID 1L
#define DEMO_PAGE_SIZE 4096ULL
#define DEMO_DMA_IOVA 0x100000000ULL

static long pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    errno = 0;
    return syscall((long)pacha_native_syscall_encode(nr), a0, a1, a2);
}

static long pacha_syscall5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    errno = 0;
    return syscall((long)pacha_native_syscall_encode(nr), a0, a1, a2, a3, a4);
}

static long pacha_capsule_query(uint64_t token, uint64_t words[PACHA_CAPSULE_SNAPSHOT_WORDS]) {
    return pacha_syscall3(
        PACHA_SYSCALL_CAPSULE_QUERY,
        token,
        (uintptr_t)words,
        PACHA_CAPSULE_SNAPSHOT_WORDS);
}

static long pacha_capsule_derive_mmio(uint64_t device, uint64_t bar, uint64_t user_va, uint64_t size, uint64_t flags) {
    return pacha_syscall5(PACHA_SYSCALL_CAPSULE_DERIVE_MMIO, device, bar, user_va, size, flags);
}

static long pacha_capsule_derive_dma_buffer(uint64_t device, uint64_t user_va, uint64_t iova, uint64_t size, uint64_t flags) {
    return pacha_syscall5(PACHA_SYSCALL_CAPSULE_DERIVE_DMA_BUFFER, device, user_va, iova, size, flags);
}

static long pacha_capsule_derive_dma_mapping_from_buffer(uint64_t dma_buffer, uint64_t iova, uint64_t size, uint64_t direction, uint64_t flags) {
    return pacha_syscall5(PACHA_SYSCALL_CAPSULE_DERIVE_DMA_MAPPING_FROM_BUFFER, dma_buffer, iova, size, direction, flags);
}

static void print_snapshot(const uint64_t words[PACHA_CAPSULE_SNAPSHOT_WORDS]) {
    printf("capsule-token=0x%016" PRIx64 "\n", words[0]);
    printf("root-token=0x%016" PRIx64 "\n", words[1]);
    printf("parent-token=0x%016" PRIx64 "\n", words[2]);
    printf("kind=%" PRIu64 " state=%" PRIu64 " rights=0x%016" PRIx64 "\n",
        words[PACHA_CAPSULE_SNAPSHOT_KIND],
        words[PACHA_CAPSULE_SNAPSHOT_STATE],
        words[PACHA_CAPSULE_SNAPSHOT_RIGHTS]);
    printf("owner=%" PRIu64 " generation=%" PRIu64 " revoke-generation=%" PRIu64 "\n",
        words[PACHA_CAPSULE_SNAPSHOT_OWNER],
        words[PACHA_CAPSULE_SNAPSHOT_GENERATION],
        words[PACHA_CAPSULE_SNAPSHOT_REVOKE_GENERATION]);
    printf("device=0x%016" PRIx64 " object=%" PRIu64 " user-va=0x%016" PRIx64 "\n",
        words[PACHA_CAPSULE_SNAPSHOT_DEVICE],
        words[PACHA_CAPSULE_SNAPSHOT_OBJECT_ID],
        words[PACHA_CAPSULE_SNAPSHOT_USER_VA]);
    printf("iova=0x%016" PRIx64 " size=%" PRIu64 " index=%" PRIu64 " flags=0x%016" PRIx64 "\n",
        words[PACHA_CAPSULE_SNAPSHOT_IOVA],
        words[PACHA_CAPSULE_SNAPSHOT_SIZE],
        words[PACHA_CAPSULE_SNAPSHOT_INDEX],
        words[PACHA_CAPSULE_SNAPSHOT_FLAGS]);
}

static int query_expect(uint64_t token, uint64_t kind, const char *label) {
    uint64_t words[PACHA_CAPSULE_SNAPSHOT_WORDS];
    memset(words, 0, sizeof(words));
    long ret = pacha_capsule_query(token, words);
    if (ret != (long)PACHA_CAPSULE_SNAPSHOT_WORDS) {
        printf("%s-query-failed ret=%ld errno=%d\n", label, ret, errno);
        return 0;
    }
    if (words[PACHA_CAPSULE_SNAPSHOT_KIND] != kind) {
        printf("%s-kind-bad kind=%" PRIu64 " expected=%" PRIu64 "\n",
            label,
            words[PACHA_CAPSULE_SNAPSHOT_KIND],
            kind);
        return 0;
    }
    printf("%s-query-ok\n", label);
    print_snapshot(words);
    return 1;
}

static int require_token_kind(uint64_t token, uint64_t kind, const char *label) {
    if (pacha_capsule_token_has_kind(token, kind)) return 1;
    printf("%s-token-kind-bad token=0x%016" PRIx64 " expected-kind=%" PRIu64 "\n", label, token, kind);
    return 0;
}

int main(int argc, char **argv) {
    uint64_t words[PACHA_CAPSULE_SNAPSHOT_WORDS];
    memset(words, 0, sizeof(words));

    if (argc == 2 && strcmp(argv[1], "env-query") == 0) {
        const char *env_token = getenv("KOBOX_PACHAOS_DEVICE_CAPSULE");
        if (env_token == NULL || env_token[0] == '\0') {
            puts("capsule-env-missing");
            return 3;
        }
        argv[1] = (char *)env_token;
        argc = 3;
        argv[2] = "query";
    }

    if (argc == 1) {
        long ret = pacha_capsule_query(0, words);
        if (ret == PACHA_SYSCALL_ERR_INVALID) {
            puts("pacha-native-syscall-ok");
            puts("capsule-invalid-token-ok");
            return 0;
        }
        if (ret == -1 && errno == ENOSYS) {
            puts("pacha-native-syscall-missing");
            return 2;
        }
        printf("capsule-invalid-token-unexpected ret=%ld errno=%d\n", ret, errno);
        return 1;
    }

    char *end = NULL;
    errno = 0;
    uint64_t token = strtoull(argv[1], &end, 0);
    if (errno != 0 || end == argv[1] || *end != '\0') {
        fprintf(stderr, "invalid capsule token: %s\n", argv[1]);
        return 64;
    }

    long ret = pacha_capsule_query(token, words);
    if (ret == -1 && errno == ENOSYS) {
        puts("pacha-native-syscall-missing");
        return 2;
    }
    if (ret != (long)PACHA_CAPSULE_SNAPSHOT_WORDS) {
        printf("device-query-failed ret=%ld errno=%d\n", ret, errno);
        return 1;
    }
    puts("device-query-ok");
    print_snapshot(words);

    if (argc > 2 && strcmp(argv[2], "query") == 0) return 0;

    long mmio_ret = pacha_capsule_derive_mmio(token, 0, 0, DEMO_PAGE_SIZE, 0);
    if (!require_token_kind((uint64_t)mmio_ret, PACHA_CAPSULE_KIND_MMIO, "mmio")) return 1;
    puts("mmio-derive-ok");
    if (!query_expect((uint64_t)mmio_ret, PACHA_CAPSULE_KIND_MMIO, "mmio")) return 1;

    void *dma_page = mmap(NULL, DEMO_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (dma_page == MAP_FAILED) {
        perror("mmap dma page");
        return 1;
    }
    memset(dma_page, 0xA5, DEMO_PAGE_SIZE);

    long dma_buffer_ret = pacha_capsule_derive_dma_buffer(token, (uintptr_t)dma_page, DEMO_DMA_IOVA, DEMO_PAGE_SIZE, 0);
    if (!require_token_kind((uint64_t)dma_buffer_ret, PACHA_CAPSULE_KIND_DMA_BUFFER, "dma-buffer")) return 1;
    puts("dma-buffer-derive-ok");
    if (!query_expect((uint64_t)dma_buffer_ret, PACHA_CAPSULE_KIND_DMA_BUFFER, "dma-buffer")) return 1;

    long dma_mapping_ret = pacha_capsule_derive_dma_mapping_from_buffer(
        (uint64_t)dma_buffer_ret,
        DEMO_DMA_IOVA,
        DEMO_PAGE_SIZE,
        PACHA_CAPSULE_DMA_BIDIRECTIONAL,
        0);
    if (!require_token_kind((uint64_t)dma_mapping_ret, PACHA_CAPSULE_KIND_DMA_MAPPING, "dma-mapping")) return 1;
    puts("dma-mapping-derive-ok");
    if (!query_expect((uint64_t)dma_mapping_ret, PACHA_CAPSULE_KIND_DMA_MAPPING, "dma-mapping")) return 1;

    puts("capsule-demo-complete");
    return 0;
}
