/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "../userland/seed0boot/src/bootstrap_abi.h"
#ifdef TEST_BOOTFS_ADAPTER
#include "../userland/kobox2_adapter/bootfs.h"
#else
#include "../userland/seed0boot/src/bootfs_reader.h"
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static unsigned char image[240];
#ifdef TEST_BOOTFS_ADAPTER
static jmp_buf failure;
_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result)
{
    (void)file; (void)line; (void)result;
    longjmp(failure, 1);
}
#endif
static void wr(unsigned offset, uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i)
        image[offset + i] = (unsigned char)(value >> (8 * i));
}
static void check(int valid)
{
#ifdef TEST_BOOTFS_ADAPTER
    if (setjmp(failure)) {
        assert(!valid);
        return;
    }
    size_t size = 0;
    const void *data = ph_bootfs_file("/a", &size);
    assert(valid && size == 3 && !memcmp(data, "abc", 3));
#else
    const unsigned char *data = NULL;
    uint32_t size = 0;
    int result = seed0_bootfs_open_file("/a", &data, &size);
    if (valid)
        assert(!result && size == 3 && !memcmp(data, "abc", 3));
    else
        assert(result == -3 && !data && !size);
#endif
}
int main(void)
{
    void *config = mmap((void *)(uintptr_t)SEED0_PROCESS_STANDARD_CONFIG_TARGET_VA,
        4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
        -1, 0);
    assert(config != MAP_FAILED);
    struct seed0_init_descriptor_page descriptor = {
        .magic = SEED0_INIT_BOOTSTRAP_MAGIC, .size = sizeof(descriptor),
        .bootfs_archive = {.flags = SEED0_INIT_BOOT_ARCHIVE_FLAG_PRESENT,
            .image_va = (uintptr_t)image, .size_bytes = sizeof(image)},
    };
    struct seed0_init_config_page *cfg = config;
    *cfg = (struct seed0_init_config_page){.magic = SEED0_INIT_CONFIG_MAGIC,
        .size = sizeof(*cfg), .descriptor_page_va = (uintptr_t)&descriptor};
    wr(0, 0x53465442, 4); wr(4, 104, 4); wr(8, sizeof(image), 8);
    wr(16, 1, 4); wr(20, 48, 4); wr(24, 104, 8); wr(32, 152, 8);
    wr(40, 2, 8); wr(48, 160, 8); wr(56, 80, 8);
    wr(104, 0, 4); wr(108, 2, 2); image[110] = 1;
    wr(112, 160, 8); wr(120, 3, 8);
    memcpy(image + 152, "/a", 2); memcpy(image + 160, "abc", 3);
    check(1);
    wr(4, 0, 4); check(0);
    wr(4, 103, 4); check(0);
    wr(4, 105, 4); check(0);
    wr(4, 1, 2); wr(6, 104, 2); check(0);
    wr(4, 104, 4); check(1);
    assert(!munmap(config, 4096));
    puts("BOOTFS reader extent/legacy rejection PASS");
    return 0;
}
