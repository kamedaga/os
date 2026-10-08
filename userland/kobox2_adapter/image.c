/* SPDX-License-Identifier: MIT */
#include "host.h"
#include "boot.h"
#include "arch/x86_64/fp_entry.h"
#if defined(PH_SANDBOX_DEVICE) && PH_SANDBOX_DEVICE
#include "device_dma.h"
#endif

#define PH_PROTECTION_MASK (KOBOX_IMAGE_READ | KOBOX_IMAGE_WRITE | KOBOX_IMAGE_EXECUTE)
#define PH_VMA_BOUNDARY 8u
#ifndef PH_CACHE_PREFLIGHT
#define PH_CACHE_PREFLIGHT 0
#endif

_Static_assert((unsigned)KOBOX_FIXED_IMAGE_READ == (unsigned)KOBOX_IMAGE_READ, "image read ABI");
_Static_assert((unsigned)KOBOX_FIXED_IMAGE_WRITE == (unsigned)KOBOX_IMAGE_WRITE, "image write ABI");
_Static_assert((unsigned)KOBOX_FIXED_IMAGE_EXECUTE == (unsigned)KOBOX_IMAGE_EXECUTE, "image execute ABI");

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
        !contains_range(image->ram_size, backing_offset, size)) {
        return 1;
    }

    PH_OK(ph_notifications_save(&previous_mask));
    ph_lock(&window->lock);
    uintptr_t requested = (uintptr_t)window->base + offset;
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, image->ram_fd, requested,
        size, protection, PACHA_MMAP_SHARED | PACHA_MMAP_FIXED, backing_offset);

    if ((uintptr_t)address != requested) {
        ph_boot_log_map_failure((uintptr_t)window->base, offset,
            backing_offset, size, protection, (uintptr_t)address);
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

static int memory_map_pages(void *opaque, size_t offset, void *backing,
    const uint64_t *page_indices, size_t page_count, unsigned protection,
    void **mapped_out) {
    struct ph_window *window = opaque;
    struct ph_image *image = backing;
    uint64_t previous_mask;
    size_t size;

    if (!window || !image || image->ram_fd < 16 || !page_indices ||
        !mapped_out || !page_count ||
        page_count > KOBOX_LINUX_MEMORY_SCATTER_PAGES ||
        (protection & ~PH_PROTECTION_MASK) != 0 ||
        (protection & KOBOX_IMAGE_READ) == 0 ||
        (protection & KOBOX_IMAGE_EXECUTE) != 0 ||
        (offset % PH_PAGE_SIZE) != 0 ||
        !contains_range(window->size, offset, page_count * PH_PAGE_SIZE)) {
        return 1;
    }
    size = page_count * PH_PAGE_SIZE;
    for (size_t i = 0; i < page_count; ++i) {
        if (page_indices[i] >= image->ram_size / PH_PAGE_SIZE) return 1;
    }

    PH_OK(ph_notifications_save(&previous_mask));
    ph_lock(&window->lock);
    uintptr_t requested = (uintptr_t)window->base + offset;
    uint64_t rights = PACHA_FD_RIGHT_CLOSE;
    if (protection & KOBOX_IMAGE_READ) rights |= PACHA_FD_RIGHT_MAP_READ;
    if (protection & KOBOX_IMAGE_WRITE) rights |= PACHA_FD_RIGHT_MAP_WRITE;
    long view_fd = pacha_syscall5(PACHA_FD_SYSCALL_VMO_CREATE_PAGE_VIEW,
        image->ram_fd, (uintptr_t)page_indices, page_count, rights,
        PACHA_FD_FLAG_CLOEXEC);
    if (view_fd < 16 || view_fd >= PACHA_FD_TABLE_LIMIT) {
        ph_boot_log_scatter_failure((uintptr_t)window->base, offset,
            page_indices[0], page_count, protection, 1, (uintptr_t)view_fd);
        ph_fail(__FILE__, __LINE__, view_fd);
    }
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, view_fd, requested,
        size, protection, PACHA_MMAP_SHARED | PACHA_MMAP_FIXED, 0);
    if ((uintptr_t)address != requested) {
        (void)pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, view_fd);
        ph_boot_log_scatter_failure((uintptr_t)window->base, offset,
            page_indices[0], page_count, protection, 2, (uintptr_t)address);
        ph_fail(__FILE__, __LINE__, address);
    }
    long closed = pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, view_fd);
    if (closed != 0) {
        ph_boot_log_scatter_failure((uintptr_t)window->base, offset,
            page_indices[0], page_count, protection, 3, (uintptr_t)closed);
        ph_fail(__FILE__, __LINE__, closed);
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

static int memory_set_cache(void *backing, size_t offset, size_t size, unsigned int cache) {
    struct ph_image *image = backing;
    if (image == NULL || image->ram_fd < 16 || size == 0 ||
        (offset | size) % PH_PAGE_SIZE != 0 || !contains_range(image->ram_size, offset, size) ||
        cache > KOBOX_LINUX_MEMORY_CACHE_WC || !image->ram_cache_modes) return 1;
    size_t page = offset / PH_PAGE_SIZE;
    const size_t end = page + size / PH_PAGE_SIZE;
    while (page < end) {
        if (image->ram_cache_modes[page] == cache) {
            ++page;
            continue;
        }
        const size_t first = page;
        do { ++page; } while (page < end && image->ram_cache_modes[page] != cache);
        const size_t bytes = (page - first) * PH_PAGE_SIZE;
        /* A large GTT BO can block in the native all-CPU cache transition.
         * Log both edges to the RAM service sink without involving the
         * kernel in userland logging or producing one line per 4 KiB page. */
        if (bytes >= 128 * 1024)
            ph_boot_log_gpu_cache_transition(0, first * PH_PAGE_SIZE,
                bytes, cache, 0);
        const long result = pacha_syscall4(PACHA_FD_SYSCALL_VMO_CACHE_POLICY,
            image->ram_fd, first * PH_PAGE_SIZE, bytes, cache);
        if (bytes >= 128 * 1024 || result != 0)
            ph_boot_log_gpu_cache_transition(1, first * PH_PAGE_SIZE,
                bytes, cache, result);
        if (result != 0) {
#if defined(PH_SANDBOX_DEVICE) && PH_SANDBOX_DEVICE
            size_t direct = 0, scattered = 0;
            uint64_t sample_iova = 0;
            int inventory_status = image->device_dma ? 0 : -1;
            for (struct ph_dma *dma = image->device_dma; dma; dma = dma->image_next) {
                size_t local_direct = 0, local_scattered = 0;
                uint64_t local_iova = 0;
                int status = ph_dma_cache_overlap(dma, first * PH_PAGE_SIZE,
                    bytes, &local_direct, &local_scattered, &local_iova);
                direct += local_direct;
                scattered += local_scattered;
                if (!sample_iova) sample_iova = local_iova;
                if (status) inventory_status = status;
            }
            ph_boot_log_gpu_cache_reject(result, first * PH_PAGE_SIZE, bytes,
                direct, scattered, sample_iova, inventory_status);
#endif
            /* The Linux host callback only tests zero/nonzero, but retaining
             * the native status lets an early preflight report the actual
             * failure instead of collapsing it into a generic boolean. */
            return (int)result;
        }
        memset(image->ram_cache_modes + first, cache, page - first);
        image->cache_drain_completed = true;
    }
    return 0;
}

static struct ph_image *cache_image;

static int memory_cache_flush(void) {
    /* set_cache completed the native all-CPU drain before publishing PAT.
     * CPA invokes this after the host has reconciled its direct aliases.
     */
    return cache_image && cache_image->cache_drain_completed ? 0 : 1;
}

const struct kobox_linux_memory_host_operations ph_memory_ops = {
    .size = sizeof(ph_memory_ops),
    .identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
    .map = memory_map,
    .map_pages = memory_map_pages,
    .reset = memory_reset,
    .protect = memory_protect,
    .set_cache = memory_set_cache,
    .cache_flush = memory_cache_flush,
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

/* CPU/IRQ queries run with Linux's FP bank live, so TLS lookup must remain
 * an integer-only leaf, including its compiler entry/return instructions.
 * Linux reads this unmasked: the fiber comes from one %fs-relative load, and
 * its state follows the task, so the pointer stays right after migration. */
static KOBOX_GPR_LEAF struct kobox_runtime_thread_state *thread_state(void *opaque) {
    (void)opaque;
    return &ph_current_fiber()->runtime;
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
        PACHA_FD_RIGHT_REVOKE | PACHA_FD_RIGHT_SHARE | PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CACHE_POLICY;

    image->ram_fd = (int)pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, image->ram_size, rights, 0);
    PH_CHECK(image->ram_fd >= 16);
    image->ram_cache_modes = ph_alloc(image->ram_size / PH_PAGE_SIZE);
    PH_CHECK(image->ram_cache_modes != NULL);
    memset(image->ram_cache_modes, KOBOX_LINUX_MEMORY_CACHE_WB,
        image->ram_size / PH_PAGE_SIZE);
    image->cache_drain_completed = false;
    cache_image = image;

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

    direct_window = reserve_window(image->ram_size);
    PH_OK(memory_map(&direct_window, 0, image, 0, image->ram_size,
        KOBOX_IMAGE_READ | KOBOX_IMAGE_WRITE, &image->direct));
    image->base = image->window.base;
    void *mapped;
    PH_OK(memory_map(&image->window, 0, image, PH_IMAGE_PHYSICAL_BASE,
        image->size, KOBOX_IMAGE_READ | KOBOX_IMAGE_WRITE, &mapped));
    PH_CHECK(mapped == image->base);
}

#if PH_CACHE_PREFLIGHT
static int cache_preflight_page(struct ph_image *image, size_t offset,
    unsigned int failed_stage, unsigned int restored_stage) {
    const int uc = memory_set_cache(image, offset, PH_PAGE_SIZE,
        KOBOX_LINUX_MEMORY_CACHE_UC_MINUS);
    if (uc != 0) {
        ph_boot_log_gpu_cache_transition(failed_stage, offset, PH_PAGE_SIZE,
            KOBOX_LINUX_MEMORY_CACHE_UC_MINUS, uc);
        return uc;
    }
    const int wb = memory_set_cache(image, offset, PH_PAGE_SIZE,
        KOBOX_LINUX_MEMORY_CACHE_WB);
    ph_boot_log_gpu_cache_transition(restored_stage, offset, PH_PAGE_SIZE,
        KOBOX_LINUX_MEMORY_CACHE_WB, wb);
    /* Never continue into Linux with a diagnostic page left in UC-. */
    if (wb != 0)
        ph_fail(__FILE__, __LINE__, (uint32_t)wb);
    return 0;
}

static void cache_preflight_before_linux(struct ph_image *image) {
    const size_t offset = 64u << 20;
    const size_t bytes = 2u << 20;
    volatile unsigned char *direct = image->direct;
    /* A boot-only native fixture cannot reproduce the live process set. This
     * separate sandbox build uses the real live VM topology but runs before
     * any Linux module or GPU driver can access the device. Keep the normal
     * product binary free of this diagnostic path. */
    for (size_t page = 0; page < offset + bytes; page += PH_PAGE_SIZE)
        direct[page] = 0;
    /* The live machine rejects the 2 MiB request while the matching QEMU
     * process topology accepts it. Two independent 4 KiB round trips tell
     * whether the refusal is per-page or specific to the larger alias plan,
     * without changing the syscall ABI or starting the GPU driver. */
    const int first = cache_preflight_page(image, offset, 0x21, 0x22);
    const int last = cache_preflight_page(image,
        offset + bytes - PH_PAGE_SIZE, 0x31, 0x32);
    if (first != 0 || last != 0)
        ph_fail(__FILE__, __LINE__, (uint32_t)(first != 0 ? first : last));
    const int uc = memory_set_cache(image, offset, bytes,
        KOBOX_LINUX_MEMORY_CACHE_UC_MINUS);
    if (uc != 0)
        ph_fail(__FILE__, __LINE__, (uint32_t)uc);
    const int wb = memory_set_cache(image, offset, bytes,
        KOBOX_LINUX_MEMORY_CACHE_WB);
    if (wb != 0)
        ph_fail(__FILE__, __LINE__, (uint32_t)wb);
}
#endif

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
    if (!image->ram_size) image->ram_size = PH_RAM_SIZE;
    PH_CHECK(!(image->ram_size % PH_PAGE_SIZE) &&
        image->ram_size > PH_IMAGE_PHYSICAL_BASE);
    PH_CHECK(kobox_fixed_image_open(file, size, PH_PAGE_SIZE, &image->fixed) ==
        KOBOX_FIXED_IMAGE_OK);
    PH_CHECK(image->fixed.link_base == KOBOX_CORE_LINK_BASE);
    PH_CHECK(image->fixed.image_size <= image->ram_size - PH_IMAGE_PHYSICAL_BASE);
    image->size = image->fixed.image_size;

    allocate_image_memory(image);
#if PH_CACHE_PREFLIGHT
    cache_preflight_before_linux(image);
#endif
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
            .boot_task = ph_current_fiber(),
            .memory = {
                .size = sizeof(layout->task.memory),
                .identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
                .operations = &ph_memory_ops,
                .ram_backing = image,
                .direct_window = &direct_window,
                .vmemmap_window = &vmemmap_window,
                .vmalloc_window = &vmalloc_window,
                .direct_map = image->direct,
                .ram_size = image->ram_size,
                .vmemmap_base = vmemmap_window.base,
                .vmemmap_size = vmemmap_window.size,
                .vmalloc_base = vmalloc_window.base,
                .vmalloc_size = vmalloc_window.size,
                .kernel_image_physical_base = PH_IMAGE_PHYSICAL_BASE,
            },
        },
    };
}
