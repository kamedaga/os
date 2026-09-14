/* SPDX-License-Identifier: MIT */
#include "host.h"
#include <elf.h>

/* Native mapping flags are adapter details, not additions to the core ABI. */
#define PH_MAP_FIXED (UINT64_C(1) << 0)
#define PH_MAP_NORESERVE (UINT64_C(1) << 5)
#define PH_PROTECTION_MASK (KOBOX_IMAGE_READ | KOBOX_IMAGE_WRITE | KOBOX_IMAGE_EXECUTE)
#define PH_VMA_BOUNDARY 8u
#define PH_CORE_TLS_MODULE 1ul

/* Process-lifetime windows for the single hosted core. */
static struct ph_window direct_window;
static struct ph_window vmemmap_window;
static struct ph_window vmalloc_window;

static bool contains_range(size_t total, size_t offset, size_t length) {
    return offset <= total && length <= total - offset;
}

static size_t page_align_up(size_t size) {
    PH_CHECK(size <= SIZE_MAX - PH_PAGE_SIZE + 1);
    return (size + PH_PAGE_SIZE - 1) & ~(PH_PAGE_SIZE - 1);
}

static const void *image_file_range(struct ph_image *image, size_t offset, size_t size) {
    PH_CHECK(contains_range(image->file_size, offset, size));
    return (const char *)image->file + offset;
}

static const Elf64_Phdr *image_program_headers(struct ph_image *image) {
    const Elf64_Ehdr *header = image->file;

    return image_file_range(image, header->e_phoff,
        (size_t)header->e_phnum * sizeof(Elf64_Phdr));
}

static struct ph_window reserve_window(size_t size) {
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, 0, 0, size, 0,
        PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS | PH_MAP_NORESERVE, 0);
    PH_CHECK(address >= (long)PH_PAGE_SIZE);

    struct ph_window window = {
        .base = (void *)(uintptr_t)address,
        .size = size,
        .pages = ph_alloc(size / PH_PAGE_SIZE + 1),
    };
    window.pages[0] = PH_VMA_BOUNDARY;
    window.pages[size / PH_PAGE_SIZE] = PH_VMA_BOUNDARY;
    return window;
}

/* Only this adapter mutates these windows. Keep native VMA splits so a later
 * mprotect can be divided into single-VMA requests. Caller holds window->lock
 * with native notifications masked; Linux supplies synchronization of users. */
static void record_mapping(struct ph_window *window, size_t offset,
    size_t size, unsigned protection) {
    size_t first_page = offset / PH_PAGE_SIZE;
    size_t end_page = (offset + size) / PH_PAGE_SIZE;

    memset(window->pages + first_page, protection, end_page - first_page);
    window->pages[first_page] |= PH_VMA_BOUNDARY;
    window->pages[end_page] |= PH_VMA_BOUNDARY;
}

static int memory_map(void *opaque, size_t offset, void *backing,
    size_t backing_offset, size_t size, unsigned protection, void **mapped_out) {
    struct ph_window *window = opaque;
    struct ph_image *image = backing;
    uint64_t previous_mask;

    if (window == NULL || image == NULL || mapped_out == NULL || size == 0 ||
        (protection & ~PH_PROTECTION_MASK) != 0 ||
        (offset | backing_offset | size) % PH_PAGE_SIZE != 0 ||
        !contains_range(window->size, offset, size) ||
        !contains_range(PH_RAM_SIZE, backing_offset, size)) {
        return 1;
    }

    PH_OK(ph_notifications_save(&previous_mask));
    ph_lock(&window->lock);
    uintptr_t requested = (uintptr_t)window->base + offset;
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, image->ram_fd, requested,
        size, protection, PACHA_MMAP_SHARED | PH_MAP_FIXED, backing_offset);

    if ((uintptr_t)address != requested) {
        ph_fail(__FILE__, __LINE__, address);
    }
    record_mapping(window, offset, size, protection);
    ph_unlock(&window->lock);
    *mapped_out = (void *)requested;
    PH_OK(ph_notifications_restore(previous_mask));
    return 0;
}

static int memory_reset(void *opaque, size_t offset, size_t size) {
    struct ph_window *window = opaque;
    uint64_t previous_mask;

    if (window == NULL || size == 0 || (offset | size) % PH_PAGE_SIZE != 0 ||
        !contains_range(window->size, offset, size)) {
        return 1;
    }

    PH_OK(ph_notifications_save(&previous_mask));
    ph_lock(&window->lock);
    uintptr_t requested = (uintptr_t)window->base + offset;
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, 0, requested, size, 0,
        PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS | PH_MAP_FIXED | PH_MAP_NORESERVE, 0);

    if ((uintptr_t)address != requested) {
        ph_fail(__FILE__, __LINE__, address);
    }
    record_mapping(window, offset, size, 0);
    ph_unlock(&window->lock);
    PH_OK(ph_notifications_restore(previous_mask));
    return 0;
}

static int memory_protect(void *opaque, size_t offset, size_t size, unsigned protection) {
    struct ph_window *window = opaque;
    uint64_t previous_mask;

    if (window == NULL || size == 0 || (protection & ~PH_PROTECTION_MASK) != 0 ||
        (offset | size) % PH_PAGE_SIZE != 0 || !contains_range(window->size, offset, size)) {
        return 1;
    }

    PH_OK(ph_notifications_save(&previous_mask));
    ph_lock(&window->lock);
    size_t first_page = offset / PH_PAGE_SIZE;
    size_t end_page = (offset + size) / PH_PAGE_SIZE;

    while (first_page < end_page) {
        size_t next_boundary = first_page + 1;

        while (next_boundary < end_page &&
            (window->pages[next_boundary] & PH_VMA_BOUNDARY) == 0) {
            ++next_boundary;
        }

        if ((window->pages[first_page] & PH_PROTECTION_MASK) != protection) {
            size_t region_offset = first_page * PH_PAGE_SIZE;
            size_t region_size = (next_boundary - first_page) * PH_PAGE_SIZE;
            long result = pacha_syscall3(PACHA_VM_SYSCALL_MPROTECT,
                (uintptr_t)window->base + region_offset, region_size, protection);

            if (result != 0) {
                ph_number("protect base", (uintptr_t)window->base);
                ph_number("protect offset", region_offset);
                ph_number("protect bytes", region_size);
                ph_number("protect flags", protection);
                ph_fail(__FILE__, __LINE__, result);
            }
            record_mapping(window, region_offset, region_size, protection);
        }
        first_page = next_boundary;
    }
    ph_unlock(&window->lock);
    PH_OK(ph_notifications_restore(previous_mask));
    return 0;
}

const struct kobox_linux_memory_host_operations ph_memory_ops = {
    .size = sizeof(ph_memory_ops),
    .identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
    .map = memory_map,
    .reset = memory_reset,
    .protect = memory_protect,
};

/* A range inside image->size may still be in a hole between PT_LOAD segments. */
static bool image_range_is_mapped(struct ph_image *image, size_t offset, size_t length) {
    const Elf64_Ehdr *header = image->file;
    const Elf64_Phdr *segments = image_program_headers(image);

    if (!contains_range(image->size, offset, length)) {
        return false;
    }

    size_t covered_until = offset;
    size_t requested_end = offset + length;

    for (unsigned i = 0; i < header->e_phnum && covered_until < requested_end; ++i) {
        const Elf64_Phdr *segment = &segments[i];

        if (segment->p_type != PT_LOAD) {
            continue;
        }
        size_t segment_end = segment->p_vaddr + page_align_up(segment->p_memsz);
        if (segment_end <= covered_until) {
            continue;
        }
        if (segment->p_vaddr > covered_until) {
            return false;
        }
        covered_until = segment_end < requested_end ? segment_end : requested_end;
    }
    return covered_until == requested_end;
}

int ph_image_protect(void *opaque, size_t offset, size_t length, unsigned protection) {
    struct ph_image *image = opaque;
    const unsigned writable_executable = KOBOX_IMAGE_WRITE | KOBOX_IMAGE_EXECUTE;

    if (length == 0 || (offset | length) % PH_PAGE_SIZE != 0 ||
        (protection & ~PH_PROTECTION_MASK) != 0 ||
        (protection & writable_executable) == writable_executable ||
        !image_range_is_mapped(image, offset, length)) {
        return 1;
    }

    PH_OK(memory_protect(&image->window, offset, length, protection));

    /* Freed init text becomes writable buddy RAM, but its image alias remains
     * revoked. Live image pages have a non-executable direct-map alias. */
    unsigned direct_protection = protection != 0
        ? protection & ~KOBOX_IMAGE_EXECUTE
        : KOBOX_IMAGE_READ | KOBOX_IMAGE_WRITE;
    return memory_protect(&direct_window, PH_IMAGE_PHYSICAL_BASE + offset,
        length, direct_protection);
}

static void *tls_address(void *opaque, unsigned long module, unsigned long offset) {
    struct ph_image *image = opaque;

    PH_CHECK(module == PH_CORE_TLS_MODULE && offset < image->tls_size);
    return (char *)ph_current_task()->tls + offset;
}

void *ph_image_lookup(void *opaque, const char *name) {
    struct ph_image *image = opaque;
    const Elf64_Ehdr *header = image->file;
    const Elf64_Shdr *sections = image_file_range(image, header->e_shoff,
        (size_t)header->e_shnum * sizeof(Elf64_Shdr));

    for (unsigned i = 0; i < header->e_shnum; ++i) {
        const Elf64_Shdr *symbol_section = &sections[i];

        if (symbol_section->sh_type != SHT_DYNSYM) {
            continue;
        }
        PH_CHECK(symbol_section->sh_entsize == sizeof(Elf64_Sym));
        PH_CHECK(symbol_section->sh_size % sizeof(Elf64_Sym) == 0);
        PH_CHECK(symbol_section->sh_link < header->e_shnum);

        const Elf64_Shdr *string_section = &sections[symbol_section->sh_link];
        PH_CHECK(string_section->sh_type == SHT_STRTAB);

        const char *strings = image_file_range(image,
            string_section->sh_offset, string_section->sh_size);
        const Elf64_Sym *symbols = image_file_range(image,
            symbol_section->sh_offset, symbol_section->sh_size);

        for (size_t j = 0; j < symbol_section->sh_size / sizeof(*symbols); ++j) {
            const Elf64_Sym *symbol = &symbols[j];

            unsigned type = ELF64_ST_TYPE(symbol->st_info);

            if (symbol->st_shndx == SHN_UNDEF ||
                (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE)) {
                continue;
            }
            PH_CHECK(symbol->st_name < string_section->sh_size);
            size_t available = string_section->sh_size - symbol->st_name;
            size_t name_length = 0;

            while (name_length < available && strings[symbol->st_name + name_length] != '\0') {
                ++name_length;
            }
            PH_CHECK(name_length < available);
            if (strcmp(strings + symbol->st_name, name) == 0) {
                PH_CHECK(image_range_is_mapped(image, symbol->st_value, symbol->st_size));
                return (char *)image->base + symbol->st_value;
            }
        }
    }
    return NULL;
}

static void validate_elf_header(struct ph_image *image) {
    const Elf64_Ehdr *header = image->file;

    PH_CHECK(image->file_size >= sizeof(*header));
    PH_CHECK(memcmp(header->e_ident, ELFMAG, SELFMAG) == 0);
    PH_CHECK(header->e_ident[EI_CLASS] == ELFCLASS64);
    PH_CHECK(header->e_ident[EI_DATA] == ELFDATA2LSB);
    PH_CHECK(header->e_ident[EI_VERSION] == EV_CURRENT && header->e_version == EV_CURRENT);
    PH_CHECK(header->e_machine == EM_X86_64 && header->e_type == ET_DYN);
    PH_CHECK(header->e_ehsize == sizeof(*header));
    PH_CHECK(header->e_phentsize == sizeof(Elf64_Phdr) && header->e_phnum != 0);
    PH_CHECK(header->e_shentsize == sizeof(Elf64_Shdr) && header->e_shnum != 0);
}

/* Validate all load segments before creating or mutating native mappings. */
static const Elf64_Phdr *inspect_image_segments(struct ph_image *image) {
    const Elf64_Ehdr *header = image->file;
    const Elf64_Phdr *segments = image_program_headers(image);
    const Elf64_Phdr *dynamic = NULL;
    const Elf64_Phdr *tls = NULL;
    size_t image_end = 0;

    for (unsigned i = 0; i < header->e_phnum; ++i) {
        const Elf64_Phdr *segment = &segments[i];

        if (segment->p_type == PT_INTERP) {
            ph_fail(__FILE__, __LINE__, PT_INTERP);
        }
        if (segment->p_type == PT_DYNAMIC) {
            PH_CHECK(dynamic == NULL);
            dynamic = segment;
        }
        if (segment->p_type == PT_TLS) {
            PH_CHECK(tls == NULL);
            tls = segment;
        }
        if (segment->p_type != PT_LOAD) {
            continue;
        }

        PH_CHECK(segment->p_vaddr % PH_PAGE_SIZE == 0 && segment->p_vaddr >= image_end);
        PH_CHECK(segment->p_filesz <= segment->p_memsz);
        PH_CHECK((segment->p_flags & (PF_X | PF_W)) != (PF_X | PF_W));
        PH_CHECK(contains_range(PH_RAM_SIZE - PH_IMAGE_PHYSICAL_BASE,
            segment->p_vaddr, page_align_up(segment->p_memsz)));
        (void)image_file_range(image, segment->p_offset, segment->p_filesz);
        image_end = segment->p_vaddr + page_align_up(segment->p_memsz);
    }

    PH_CHECK(image_end != 0 && dynamic != NULL && tls != NULL);
    PH_CHECK(tls->p_memsz != 0 && tls->p_filesz <= tls->p_memsz);
    PH_CHECK(tls->p_align != 0 && (tls->p_align & (tls->p_align - 1)) == 0);
    PH_CHECK(tls->p_align <= PH_PAGE_SIZE);
    image->size = image_end;
    image->tls_size = tls->p_memsz;
    image->tls_filesz = tls->p_filesz;
    image->tls_align = tls->p_align;
    image->tls_template = image_file_range(image, tls->p_offset, tls->p_filesz);
    return dynamic;
}

static void allocate_image_memory(struct ph_image *image) {
    const uint64_t rights = PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC | PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_REVOKE | PACHA_FD_RIGHT_SHARE | PACHA_FD_RIGHT_INSPECT;

    image->ram_fd = (int)pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, PH_RAM_SIZE, rights, 0);
    PH_CHECK(image->ram_fd >= 16);
    direct_window = reserve_window(PH_RAM_SIZE);
    PH_OK(memory_map(&direct_window, 0, image, 0, PH_RAM_SIZE,
        KOBOX_IMAGE_READ | KOBOX_IMAGE_WRITE, &image->direct));
    image->window = reserve_window(image->size);
    image->base = image->window.base;
}

static void copy_load_segments(struct ph_image *image) {
    const Elf64_Ehdr *header = image->file;
    const Elf64_Phdr *segments = image_program_headers(image);

    for (unsigned i = 0; i < header->e_phnum; ++i) {
        const Elf64_Phdr *segment = &segments[i];
        void *mapped;

        if (segment->p_type != PT_LOAD || segment->p_memsz == 0) {
            continue;
        }
        PH_OK(memory_map(&image->window, segment->p_vaddr, image,
            PH_IMAGE_PHYSICAL_BASE + segment->p_vaddr, page_align_up(segment->p_memsz),
            KOBOX_IMAGE_READ | KOBOX_IMAGE_WRITE, &mapped));
        memcpy(mapped, image_file_range(image, segment->p_offset, segment->p_filesz),
            segment->p_filesz);
    }
}

static void relocate_image(struct ph_image *image, const Elf64_Phdr *dynamic_segment) {
    PH_CHECK(image_range_is_mapped(image, dynamic_segment->p_vaddr, dynamic_segment->p_memsz));
    PH_CHECK(dynamic_segment->p_memsz % sizeof(Elf64_Dyn) == 0);

    const Elf64_Dyn *dynamic = (void *)((char *)image->base + dynamic_segment->p_vaddr);
    size_t relocation_offset = 0;
    size_t relocation_bytes = 0;
    size_t relocation_entry_size = 0;
    bool terminated = false;

    for (size_t i = 0; i < dynamic_segment->p_memsz / sizeof(*dynamic); ++i) {
        Elf64_Sxword tag = dynamic[i].d_tag;

        if (tag == DT_NULL) {
            terminated = true;
            break;
        }
        PH_CHECK(tag != DT_NEEDED && tag != DT_REL && tag != DT_JMPREL &&
            tag != DT_INIT && tag != DT_INIT_ARRAY && tag != DT_RELR);
        if (tag == DT_RELA) {
            relocation_offset = dynamic[i].d_un.d_ptr;
        }
        if (tag == DT_RELASZ) {
            relocation_bytes = dynamic[i].d_un.d_val;
        }
        if (tag == DT_RELAENT) {
            relocation_entry_size = dynamic[i].d_un.d_val;
        }
    }

    PH_CHECK(terminated && relocation_entry_size == sizeof(Elf64_Rela));
    PH_CHECK(relocation_bytes % relocation_entry_size == 0);
    PH_CHECK(image_range_is_mapped(image, relocation_offset, relocation_bytes));
    const Elf64_Rela *relocations = (void *)((char *)image->base + relocation_offset);

    for (size_t i = 0; i < relocation_bytes / relocation_entry_size; ++i) {
        const Elf64_Rela *relocation = &relocations[i];
        uint64_t value;

        PH_CHECK(image_range_is_mapped(image, relocation->r_offset, sizeof(value)));
        switch (ELF64_R_TYPE(relocation->r_info)) {
        case R_X86_64_RELATIVE:
            /* B + A is not necessarily an image pointer: initial page tables
             * include address flags/biases. Only the destination is bounded. */
            PH_CHECK(ELF64_R_SYM(relocation->r_info) == 0);
            value = (uint64_t)(uintptr_t)image->base + (uint64_t)relocation->r_addend;
            break;
        case R_X86_64_DTPMOD64:
            PH_CHECK(relocation->r_addend == 0);
            value = PH_CORE_TLS_MODULE;
            break;
        default:
            ph_fail(__FILE__, __LINE__, ELF64_R_TYPE(relocation->r_info));
        }
        memcpy((char *)image->base + relocation->r_offset, &value, sizeof(value));
    }
}

static void protect_load_segments(struct ph_image *image) {
    const Elf64_Ehdr *header = image->file;
    const Elf64_Phdr *segments = image_program_headers(image);

    for (unsigned i = 0; i < header->e_phnum; ++i) {
        const Elf64_Phdr *segment = &segments[i];
        unsigned protection = 0;

        if (segment->p_type != PT_LOAD || segment->p_memsz == 0) {
            continue;
        }
        if ((segment->p_flags & PF_R) != 0) {
            protection |= KOBOX_IMAGE_READ;
        }
        if ((segment->p_flags & PF_W) != 0) {
            protection |= KOBOX_IMAGE_WRITE;
        }
        if ((segment->p_flags & PF_X) != 0) {
            protection |= KOBOX_IMAGE_EXECUTE;
        }
        PH_OK(memory_protect(&image->window, segment->p_vaddr,
            page_align_up(segment->p_memsz), protection));
    }
}

static void bind_core_runtime(struct ph_image *image) {
    struct ph_task *boot_thread = ph_current_task();

    /* Boot-thread TLS must exist before calling even runtime_bind in the core. */
    boot_thread->tls = ph_alloc(image->tls_size);
    boot_thread->tls_size = image->tls_size;
    memcpy(boot_thread->tls, image->tls_template, image->tls_filesz);

    struct kobox_runtime_host host = {
        .size = sizeof(host),
        .context = image,
        .tls_address = tls_address,
    };
    PH_CHECK(kobox_boot_core_prepare(&image->core, image, ph_image_lookup, &host) ==
        KOBOX_BOOT_CORE_OK);
}

int ph_image_open(struct ph_image *image, const void *file, size_t size) {
    image->file = file;
    image->file_size = size;

    validate_elf_header(image);
    const Elf64_Phdr *dynamic = inspect_image_segments(image);

    allocate_image_memory(image);
    copy_load_segments(image);
    relocate_image(image, dynamic);
    protect_load_segments(image);
    bind_core_runtime(image);

    ph_number("core image bytes", image->size);
    ph_number("core TLS bytes", image->tls_size);
    return 0;
}

void ph_layout_init(struct ph_image *image, struct kobox_linux_boot_layout *layout) {
    vmemmap_window = reserve_window(16ul << 20);
    vmalloc_window = reserve_window(256ul << 20);
    *layout = (struct kobox_linux_boot_layout) {
        .size = sizeof(*layout),
        .exceptions_install = ph_exceptions_install,
        .image_protect = ph_image_protect,
        .image = image,
        .command_line = "console=kobox earlycon loglevel=8",
        .task = {
            .size = sizeof(layout->task),
            .identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
            .operations = &ph_task_ops,
            .boot_task = ph_current_task(),
            .memory = {
                .size = sizeof(layout->task.memory),
                .identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
                .operations = &ph_memory_ops,
                .ram_backing = image,
                .direct_window = &direct_window,
                .vmemmap_window = &vmemmap_window,
                .vmalloc_window = &vmalloc_window,
                .direct_map = image->direct,
                .ram_size = PH_RAM_SIZE,
                .vmemmap_base = vmemmap_window.base,
                .vmemmap_size = vmemmap_window.size,
                .vmalloc_base = vmalloc_window.base,
                .vmalloc_size = vmalloc_window.size,
                .kernel_image_physical_base = PH_IMAGE_PHYSICAL_BASE,
            },
        },
    };
}
