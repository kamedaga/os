/* SPDX-License-Identifier: MIT */
/* Native bootstrap loading; no Linux ELF, VMA or page-allocation policy. */
#include "vm_internal.h"
#include <elf.h>
#include <errno.h>

int ph_vm_native_error(long status) {
    switch (status) {
    case PACHA_SYSCALL_OK:
        return 0;
    case PACHA_SYSCALL_ERR_INVALID:
        return -EINVAL;
    case PACHA_SYSCALL_ERR_NOT_READY:
        return -EAGAIN;
    case PACHA_SYSCALL_ERR_ALLOC:
        return -ENOMEM;
    case PACHA_SYSCALL_ERR_MAP:
        return -EFAULT;
    case PACHA_SYSCALL_ERR_CLOSED:
        return -ESRCH;
    default:
        return -EIO;
    }
}

void ph_vm_event_signal(int fd) {
    const uint64_t one = 1;

    PH_CHECK(pacha_syscall3(PACHA_FD_SYSCALL_WRITE, fd,
        (uintptr_t)&one, sizeof(one)) == sizeof(one));
}

void ph_vm_event_consume(int fd) {
    uint64_t count;

    PH_CHECK(pacha_syscall3(PACHA_FD_SYSCALL_READ, fd,
        (uintptr_t)&count, sizeof(count)) == sizeof(count));
    PH_CHECK(count == 1);
}

void ph_vm_map_anonymous(int process_fd, uint64_t address, size_t length) {
    long mapped = pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, process_fd, 0,
        address, length, PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_PROCESS_MAP_ANONYMOUS | PACHA_PROCESS_MAP_PRIVATE);

    PH_CHECK((uint64_t)mapped == address);
}

static bool file_range(size_t file_size, uint64_t offset, uint64_t length) {
    return offset <= file_size && length <= file_size - offset;
}

static unsigned segment_protection(unsigned flags) {
    unsigned protection = 0;

    if (flags & PF_R) {
        protection |= PACHA_PROT_READ;
    }
    if (flags & PF_W) {
        protection |= PACHA_PROT_WRITE;
    }
    if (flags & PF_X) {
        protection |= PACHA_PROT_EXEC;
    }
    return protection;
}

uint64_t ph_vm_load_bootstrap(int process_fd, const void *file, size_t file_size) {
    Elf64_Ehdr header;
    bool entry_executable = false;
    uint64_t previous_end = PH_VM_CLIENT_IMAGE_BASE;

    PH_CHECK(file_size >= sizeof(header));
    memcpy(&header, file, sizeof(header));
    PH_CHECK(memcmp(header.e_ident, ELFMAG, SELFMAG) == 0);
    PH_CHECK(header.e_ident[EI_CLASS] == ELFCLASS64 && header.e_ident[EI_DATA] == ELFDATA2LSB);
    PH_CHECK(header.e_type == ET_EXEC && header.e_machine == EM_X86_64);
    PH_CHECK(header.e_phentsize == sizeof(Elf64_Phdr) && header.e_phnum != 0);
    PH_CHECK(file_range(file_size, header.e_phoff, (uint64_t)header.e_phnum * sizeof(Elf64_Phdr)));

    /* Validate the entire trusted image before creating any mapping. Ordered,
     * page-separated PT_LOADs are an explicit property of vm_client.ld. */
    for (unsigned index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr segment;

        memcpy(&segment, (const char *)file + header.e_phoff + index * sizeof(segment),
            sizeof(segment));
        PH_CHECK(segment.p_type == PT_LOAD);
        PH_CHECK(segment.p_filesz <= segment.p_memsz);
        PH_CHECK(file_range(file_size, segment.p_offset, segment.p_filesz));
        PH_CHECK((segment.p_vaddr & (PH_PAGE_SIZE - 1)) == 0);
        PH_CHECK(segment.p_vaddr >= previous_end && segment.p_vaddr < PH_VM_CLIENT_IMAGE_LIMIT);
        PH_CHECK(segment.p_memsz <= PH_VM_CLIENT_IMAGE_LIMIT - segment.p_vaddr);
        PH_CHECK((segment.p_flags & ~(PF_R | PF_W | PF_X)) == 0);
        PH_CHECK((segment.p_flags & (PF_W | PF_X)) != (PF_W | PF_X));
        previous_end = segment.p_vaddr +
            ((segment.p_memsz + PH_PAGE_SIZE - 1) & ~(PH_PAGE_SIZE - 1));
        if ((segment.p_flags & PF_X) && header.e_entry >= segment.p_vaddr &&
            header.e_entry - segment.p_vaddr < segment.p_memsz) {
            entry_executable = true;
        }
    }
    PH_CHECK(entry_executable);

    for (unsigned index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr segment;

        memcpy(&segment, (const char *)file + header.e_phoff + index * sizeof(segment),
            sizeof(segment));
        if (!segment.p_memsz) {
            continue;
        }
        size_t length = (segment.p_memsz + PH_PAGE_SIZE - 1) & ~(PH_PAGE_SIZE - 1);
        unsigned protection = segment_protection(segment.p_flags);
        uint64_t rights = PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE |
            PACHA_FD_RIGHT_MAP_EXEC | PACHA_FD_RIGHT_CLOSE;
        long backing = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, length, rights, 0);

        PH_CHECK(backing >= 16);
        long alias = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, backing, 0, length,
            PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);

        PH_CHECK(alias >= (long)PH_PAGE_SIZE);
        memcpy((void *)(uintptr_t)alias, (const char *)file + segment.p_offset, segment.p_filesz);
        PH_CHECK((uint64_t)pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, process_fd,
            backing, segment.p_vaddr, length, protection, PACHA_PROCESS_MAP_SHARED) ==
            segment.p_vaddr);
        ph_free((void *)(uintptr_t)alias, length);
        PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, backing));
    }
    return header.e_entry;
}
