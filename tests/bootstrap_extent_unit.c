#define _GNU_SOURCE
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "bootstrap_abi.h"
#include "pacha/abi.h"

int main(void)
{
    _Static_assert(offsetof(struct seed0_init_descriptor_page, boot_images) == 520,
        "C mirror must include the kernel producer's complete tail");
    _Static_assert(offsetof(struct pacha_kernel_log_read, bytes) == 72,
        "log payload offset must not depend on unchecked alignment padding");
    const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    void *config_memory = mmap((void *)(uintptr_t)SEED0_PROCESS_STANDARD_CONFIG_TARGET_VA,
        page_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(config_memory != MAP_FAILED);
    struct seed0_init_config_page *config = config_memory;
    struct seed0_init_descriptor_page descriptor = {
        .magic = SEED0_INIT_BOOTSTRAP_MAGIC,
        .size = sizeof(descriptor),
    };
    *config = (struct seed0_init_config_page){
        .magic = SEED0_INIT_CONFIG_MAGIC,
        .size = sizeof(*config),
        .descriptor_page_va = (uint64_t)(uintptr_t)&descriptor,
    };
    assert(seed0_bootstrap_descriptor() == &descriptor);
    config->size--;
    assert(seed0_bootstrap_descriptor() == NULL);
    config->size = sizeof(*config) + 1;
    assert(seed0_bootstrap_descriptor() == NULL);
    config->size = sizeof(*config);
    config->reserved0 = UINT64_C(1) << 32;
    assert(seed0_bootstrap_descriptor() == NULL);
    config->reserved0 = 0;
    descriptor.size = offsetof(struct seed0_init_descriptor_page, boot_images);
    assert(seed0_bootstrap_descriptor() == NULL);
    descriptor.size = sizeof(descriptor) + 1;
    assert(seed0_bootstrap_descriptor() == NULL);
    descriptor.size = sizeof(descriptor);
    assert(seed0_bootstrap_descriptor() == &descriptor);
    assert(munmap(config_memory, page_size) == 0);
    puts("bootstrap extent layout and rejection: PASS");
    return 0;
}
