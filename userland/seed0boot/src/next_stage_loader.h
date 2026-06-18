#ifndef SEED0BOOT_NEXT_STAGE_LOADER_H
#define SEED0BOOT_NEXT_STAGE_LOADER_H

#include <stdint.h>

struct seed0_loaded_process {
    int process_fd;
    uint64_t runtime_entry;
    uint64_t phdr_va;
    uint64_t phent;
    uint64_t phnum;
    uint16_t load_segments;
};

int seed0_load_elf_process(
    const char *path,
    const unsigned char *image,
    uint32_t image_size,
    struct seed0_loaded_process *out);
int seed0_map_bytes_into_process(
    int process_fd,
    uint64_t target_va,
    const void *data,
    uint64_t size,
    uint64_t prot);
int seed0_start_process(const struct seed0_loaded_process *loaded, const char *argv0);
int seed0_stage_next_elf(const char *path, const unsigned char *image, uint32_t image_size);

#endif
