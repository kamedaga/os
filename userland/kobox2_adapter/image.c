/* SPDX-License-Identifier: MIT */
#include "host.h"

#define PH_PROTECTION_MASK (KOBOX_IMAGE_READ | KOBOX_IMAGE_WRITE | KOBOX_IMAGE_EXECUTE)
#define PH_VMA_BOUNDARY 8u

_Static_assert(KOBOX_FIXED_IMAGE_READ == KOBOX_IMAGE_READ, "image read ABI");
_Static_assert(KOBOX_FIXED_IMAGE_WRITE == KOBOX_IMAGE_WRITE, "image write ABI");
_Static_assert(KOBOX_FIXED_IMAGE_EXECUTE == KOBOX_IMAGE_EXECUTE, "image execute ABI");

/* Process-lifetime windows for the single hosted core. */
static struct ph_window direct_window;
static struct ph_window vmemmap_window;
static struct ph_window vmalloc_window;

static bool contains_range(size_t total, size_t offset, size_t length) {
    return offset <= total && length <= total - offset;
}

static struct ph_window reserve_window_mapping(void *requested, size_t size) {
    uint64_t flags = PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS | PACHA_MMAP_NORESERVE;

    if (requested) {
        flags |= PACHA_MMAP_FIXED_NOREPLACE;
    }
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, 0, (uintptr_t)requested,
        size, 0, flags, 0);
    if (address < (long)PH_PAGE_SIZE ||
        (requested != NULL && (uintptr_t)address != (uintptr_t)requested)) {
        ph_number("reserve requested", (uintptr_t)requested);
        ph_number("reserve bytes", size);
        ph_number("reserve result", (uintptr_t)address);
        ph_fail(__FILE__, __LINE__, address);
    }

    return (struct ph_window) {
        .base = (void *)(uintptr_t)address,
        .size = size,
    };
}

static void initialize_window_tracking(struct ph_window *window) {
    PH_CHECK(window != NULL && window->base != NULL && window->size != 0 &&
        window->pages == NULL);
    window->pages = ph_alloc(window->size / PH_PAGE_SIZE + 1);
    window->pages[0] = PH_VMA_BOUNDARY;
    window->pages[window->size / PH_PAGE_SIZE] = PH_VMA_BOUNDARY;
}

static struct ph_window reserve_window_at(void *requested, size_t size) {
    struct ph_window window = reserve_window_mapping(requested, size);

    initialize_window_tracking(&window);
    return window;
}

static struct ph_window reserve_window(size_t size) {
    return reserve_window_at(NULL, size);
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
        size, protection, PACHA_MMAP_SHARED | PACHA_MMAP_FIXED, backing_offset);

    if ((uintptr_t)address != requested) {
        ph_number("map window", (uintptr_t)window->base);
        ph_number("map offset", offset);
        ph_number("map backing", backing_offset);
        ph_number("map bytes", size);
        ph_number("map protection", protection);
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
        PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS | PACHA_MMAP_FIXED |
            PACHA_MMAP_NORESERVE, 0);

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
    if (!contains_range(image->size, offset, length)) {
        return false;
    }

    size_t covered_until = offset;
    size_t requested_end = offset + length;

    for (unsigned i = 0; i < image->fixed.segment_count && covered_until < requested_end; ++i) {
        const struct kobox_fixed_image_segment *segment = &image->fixed.segments[i];
        size_t segment_end = segment->image_offset + segment->mapping_size;

        if (segment_end <= covered_until) {
            continue;
        }
        if (segment->image_offset > covered_until) {
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

static struct kobox_runtime_thread_state *thread_state(void *opaque) {
    (void)opaque;
    return &ph_current_task()->runtime;
}

void *ph_image_lookup(void *opaque, const char *name) {
    struct ph_image *image = opaque;
    size_t offset;

    if (kobox_fixed_image_symbol(&image->fixed, name, &offset) != KOBOX_FIXED_IMAGE_OK) {
        return NULL;
    }
    return (char *)image->base + offset;
}

static void allocate_image_memory(struct ph_image *image) {
    const uint64_t rights = PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC | PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_REVOKE | PACHA_FD_RIGHT_SHARE | PACHA_FD_RIGHT_INSPECT;

    image->ram_fd = (int)pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, PH_RAM_SIZE, rights, 0);
    PH_CHECK(image->ram_fd >= 16);

    /* Reserve every fixed core address before ph_alloc or a randomized window
     * can occupy one of them. Tracking metadata is allocated only after all
     * three reservations are present. */
    image->window = reserve_window_mapping(
        (void *)(uintptr_t)image->fixed.link_base, image->size);
    vmemmap_window = reserve_window_mapping(
        (void *)(uintptr_t)KOBOX_CORE_VMEMMAP_BASE, KOBOX_CORE_VMEMMAP_SIZE);
    vmalloc_window = reserve_window_mapping(
        (void *)(uintptr_t)KOBOX_CORE_VMALLOC_BASE, KOBOX_CORE_VMALLOC_SIZE);
    initialize_window_tracking(&image->window);
    initialize_window_tracking(&vmemmap_window);
    initialize_window_tracking(&vmalloc_window);

    direct_window = reserve_window(PH_RAM_SIZE);
    PH_OK(memory_map(&direct_window, 0, image, 0, PH_RAM_SIZE,
        KOBOX_IMAGE_READ | KOBOX_IMAGE_WRITE, &image->direct));
    image->base = image->window.base;
    void *mapped;
    PH_OK(memory_map(&image->window, 0, image, PH_IMAGE_PHYSICAL_BASE,
        image->size, KOBOX_IMAGE_READ | KOBOX_IMAGE_WRITE, &mapped));
    PH_CHECK(mapped == image->base);
}

static void copy_load_segments(struct ph_image *image) {
    for (unsigned i = 0; i < image->fixed.segment_count; ++i) {
        const struct kobox_fixed_image_segment *segment = &image->fixed.segments[i];

        memcpy((char *)image->base + segment->image_offset,
            image->fixed.file + segment->file_offset, segment->file_size);
    }
}

static void protect_load_segments(struct ph_image *image) {
    PH_OK(memory_protect(&image->window, 0, image->size, 0));
    for (unsigned i = 0; i < image->fixed.segment_count; ++i) {
        const struct kobox_fixed_image_segment *segment = &image->fixed.segments[i];

        PH_OK(memory_protect(&image->window, segment->image_offset,
            segment->mapping_size, segment->protection));
    }
}

static void bind_core_runtime(struct ph_image *image) {
    struct kobox_runtime_host host = {
        .size = sizeof(host),
        .context = image,
        .thread_state = thread_state,
    };
    PH_CHECK(kobox_boot_core_prepare(&image->core, image, ph_image_lookup, &host) ==
        KOBOX_BOOT_CORE_OK);
}

int ph_image_open(struct ph_image *image, const void *file, size_t size) {
    PH_CHECK(image && file);
    PH_CHECK(kobox_fixed_image_open(file, size, PH_PAGE_SIZE, &image->fixed) ==
        KOBOX_FIXED_IMAGE_OK);
    PH_CHECK(image->fixed.link_base == KOBOX_CORE_LINK_BASE);
    PH_CHECK(image->fixed.image_size <= PH_RAM_SIZE - PH_IMAGE_PHYSICAL_BASE);
    image->size = image->fixed.image_size;

    allocate_image_memory(image);
    copy_load_segments(image);
    protect_load_segments(image);
    bind_core_runtime(image);

    ph_number("core image bytes", image->size);
    return 0;
}

void ph_layout_init(struct ph_image *image, struct kobox_linux_boot_layout *layout) {
    PH_CHECK(vmemmap_window.base != NULL && vmalloc_window.base != NULL);
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
