/* SPDX-License-Identifier: MIT */
/* Private executable image preparation. The hosted Linux loader still owns
 * ELF placement, auxiliary vectors, credentials, stack and file mappings. */
#include "host.h"
#include "exec_image.h"
#include "patch_scan.h"
#include <elf.h>

struct ph_exec_image ph_exec_prepare_image(const void *file, size_t length) {
    Elf64_Ehdr header;

    PH_CHECK(length >= sizeof(header));
    memcpy(&header, file, sizeof(header));
    PH_CHECK(!memcmp(header.e_ident, ELFMAG, SELFMAG));
    PH_CHECK(header.e_ident[EI_CLASS] == ELFCLASS64 && header.e_ident[EI_DATA] == ELFDATA2LSB);
    PH_CHECK(header.e_machine == EM_X86_64 &&
        (header.e_type == ET_EXEC || header.e_type == ET_DYN));
    PH_CHECK(header.e_phentsize == sizeof(Elf64_Phdr) && header.e_phoff <= length);
    PH_CHECK(header.e_phnum <= (length - header.e_phoff) / sizeof(Elf64_Phdr));

    unsigned char *image = ph_alloc(length);
    uint64_t patched = 0;

    memcpy(image, file, length);
    for (unsigned index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr segment;

        memcpy(&segment, image + header.e_phoff + index * sizeof(segment), sizeof(segment));
        if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_X)) {
            continue;
        }
        PH_CHECK(segment.p_offset <= length && segment.p_filesz <= length - segment.p_offset);
        lpr_patch_scan_result_t result;

        /* Reuse the existing Zydis decoder. Byte matches and data literals
         * are not enough to authorize rewriting an instruction. */
        PH_OK(lpr_patch_scan_syscalls(image + segment.p_offset, segment.p_filesz, 0, &result));
        PH_CHECK(!result.failed_sites);
        patched += result.patched_sites;
    }
    ph_number("ELF LPR patched instructions", patched);
    PH_CHECK(patched <= SIZE_MAX / sizeof(uint64_t));
    struct ph_exec_image prepared = {
        .bytes = image, .length = length, .elf_type = header.e_type,
        .syscall_sites = patched ? ph_alloc(patched * sizeof(uint64_t)) : NULL,
    };

    /* Record only bytes changed by the validated decoder above, never an
     * arbitrary CALL found in client data or an existing CALL instruction. */
    for (unsigned index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr segment;

        memcpy(&segment, image + header.e_phoff + index * sizeof(segment), sizeof(segment));
        if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_X)) {
            continue;
        }
        const unsigned char *before = (const unsigned char *)file + segment.p_offset;
        const unsigned char *after = image + segment.p_offset;

        for (uint64_t offset = 0; offset + 1 < segment.p_filesz; ++offset) {
            if (before[offset] != 0x0f || before[offset + 1] != 0x05 ||
                after[offset] != 0xff || after[offset + 1] != 0xd0) {
                continue;
            }
            PH_CHECK(prepared.site_count < patched && segment.p_vaddr <= UINT64_MAX - offset);
            prepared.syscall_sites[prepared.site_count++] = segment.p_vaddr + offset;
            ++offset;
        }
    }
    PH_CHECK(prepared.site_count == patched);
    return prepared;
}

void ph_exec_release_image(struct ph_exec_image *image) {
    if (image->syscall_sites) {
        ph_free(image->syscall_sites, image->site_count * sizeof(uint64_t));
    }
    ph_free(image->bytes, image->length);
    *image = (struct ph_exec_image){0};
}
