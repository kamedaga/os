#include "capc.h"

#include "cap_errno.h"
#include "cap_syscall.h"

#define CAP_EBADF 9
#define CAP_HEAP_BASE_VA ((uintptr_t)0x30000000ull)
#define CAP_PAGE_SIZE ((uintptr_t)4096u)
#define CAP_ALIGN ((size_t)16u)

static uintptr_t cap_heap_cur = CAP_HEAP_BASE_VA;
static uintptr_t cap_heap_end = CAP_HEAP_BASE_VA;
static uintptr_t cap_heap_next_map_va = CAP_HEAP_BASE_VA;

struct cap_block {
    size_t size;
    int free;
    struct cap_block *next;
};

static struct cap_block *cap_heap_head = 0;
static struct cap_block *cap_heap_tail = 0;

static size_t cap_align_up(size_t value, size_t align) {
    size_t rem = value & (align - 1u);
    if (rem == 0u) {
        return value;
    }
    return value + (align - rem);
}

static int cap_sys_is_error(uint64_t v) {
    return v != 0 && v <= CAP_SYSCALL_ERR_EMPTY;
}

static int cap_untyped_alloc_raw(size_t bytes, size_t align, uint64_t flags, uint64_t *out_token) {
    uint64_t token;
    if (out_token == 0 || bytes == 0) {
        cap_errno = CAP_EINVAL;
        return -1;
    }

    token = cap_syscall3(
        CAP_SYSCALL_UNTYPED_ALLOC,
        (uint64_t)bytes,
        (uint64_t)align,
        flags);
    if (cap_sys_is_error(token)) {
        cap_errno = cap_sys_status_to_errno(token);
        return -1;
    }

    *out_token = token;
    return 0;
}

static int cap_heap_map_until(uintptr_t end_addr) {
    while (end_addr > cap_heap_end) {
        uint64_t paddr = cap_syscall0(CAP_SYSCALL_ALLOC_PAGE);
        uint64_t map_status;
        if (cap_sys_is_error(paddr)) {
            cap_errno = cap_sys_status_to_errno(paddr);
            return -1;
        }

        map_status = cap_syscall3(
            CAP_SYSCALL_MAP_PAGE,
            (uint64_t)cap_heap_next_map_va,
            paddr,
            1u);
        if (map_status != CAP_SYSCALL_OK) {
            cap_errno = cap_sys_status_to_errno(map_status);
            return -1;
        }

        cap_heap_next_map_va += CAP_PAGE_SIZE;
        cap_heap_end += CAP_PAGE_SIZE;
    }
    return 0;
}

static struct cap_block *cap_find_fit(size_t need) {
    struct cap_block *cur = cap_heap_head;
    while (cur != 0) {
        if (cur->free && cur->size >= need) {
            return cur;
        }
        cur = cur->next;
    }
    return 0;
}

static void cap_split_block(struct cap_block *blk, size_t need) {
    size_t hdr = cap_align_up(sizeof(struct cap_block), CAP_ALIGN);
    if (blk->size <= need + hdr + CAP_ALIGN) {
        return;
    }

    {
        uintptr_t base = (uintptr_t)blk;
        uintptr_t next_addr = base + hdr + need;
        struct cap_block *next_blk = (struct cap_block *)next_addr;
        next_blk->size = blk->size - need - hdr;
        next_blk->free = 1;
        next_blk->next = blk->next;
        blk->size = need;
        blk->next = next_blk;
        if (cap_heap_tail == blk) {
            cap_heap_tail = next_blk;
        }
    }
}

static void cap_coalesce_forward(struct cap_block *blk) {
    size_t hdr = cap_align_up(sizeof(struct cap_block), CAP_ALIGN);
    while (blk->next != 0 && blk->next->free) {
        struct cap_block *n = blk->next;
        blk->size += hdr + n->size;
        blk->next = n->next;
        if (cap_heap_tail == n) {
            cap_heap_tail = blk;
        }
    }
}

long cap_log(const char *buf, size_t len) {
    if (buf == 0 && len != 0) {
        cap_errno = CAP_EFAULT;
        return -1;
    }

    {
        uint64_t status = cap_syscall2(
            CAP_SYSCALL_LOG,
            (uint64_t)(uintptr_t)buf,
            (uint64_t)len);
        if (status != CAP_SYSCALL_OK) {
            cap_errno = cap_sys_status_to_errno(status);
            return -1;
        }
    }
    return (long)len;
}

long cap_write(int fd, const void *buf, size_t len) {
    if (fd != 1 && fd != 2) {
        cap_errno = CAP_EBADF;
        return -1;
    }
    return cap_log((const char *)buf, len);
}

int cap_untyped_alloc(size_t bytes, size_t align, uint64_t flags, uint64_t *out_token) {
    return cap_untyped_alloc_raw(bytes, align, flags, out_token);
}

int cap_untyped_retype_pages(
    uint64_t token,
    void *base_va,
    size_t page_count,
    uint64_t flags,
    uint64_t *out_paddrs
) {
    uint64_t status;
    if (token == 0 || base_va == 0 || page_count == 0) {
        cap_errno = CAP_EINVAL;
        return -1;
    }

    status = cap_syscall5(
        CAP_SYSCALL_UNTYPED_RETYPE_PAGES,
        token,
        (uint64_t)(uintptr_t)base_va,
        (uint64_t)page_count,
        flags,
        (uint64_t)(uintptr_t)out_paddrs);
    return cap_status_to_ret(status, 0);
}

int cap_untyped_alloc_map_pages(
    void *base_va,
    size_t page_count,
    uint64_t flags,
    uint64_t *out_paddrs
) {
    uint64_t status;
    if (base_va == 0 || page_count == 0) {
        cap_errno = CAP_EINVAL;
        return -1;
    }

    status = cap_syscall4(
        CAP_SYSCALL_UNTYPED_ALLOC_MAP_PAGES,
        (uint64_t)(uintptr_t)base_va,
        (uint64_t)page_count,
        flags,
        (uint64_t)(uintptr_t)out_paddrs);
    return cap_status_to_ret(status, 0);
}

int cap_untyped_reset(uint64_t token) {
    uint64_t status;
    if (token == 0) {
        cap_errno = CAP_EINVAL;
        return -1;
    }

    status = cap_syscall1(CAP_SYSCALL_UNTYPED_RESET, token);
    return cap_status_to_ret(status, 0);
}

void *cap_malloc(size_t len) {
    size_t need = cap_align_up((len == 0) ? 1 : len, CAP_ALIGN);
    size_t hdr = cap_align_up(sizeof(struct cap_block), CAP_ALIGN);
    struct cap_block *blk = cap_find_fit(need);

    if (blk != 0) {
        blk->free = 0;
        cap_split_block(blk, need);
        return (void *)((uintptr_t)blk + hdr);
    }

    {
        uintptr_t block_addr = cap_heap_cur;
        uintptr_t payload_addr = block_addr + hdr;
        uintptr_t block_end = payload_addr + need;
        if (cap_heap_map_until(block_end) != 0) {
            return 0;
        }

        blk = (struct cap_block *)block_addr;
        blk->size = need;
        blk->free = 0;
        blk->next = 0;
        if (cap_heap_tail != 0) {
            cap_heap_tail->next = blk;
            cap_heap_tail = blk;
        } else {
            cap_heap_head = blk;
            cap_heap_tail = blk;
        }
        cap_heap_cur = block_end;
        return (void *)payload_addr;
    }
}

void cap_free(void *ptr) {
    size_t hdr = cap_align_up(sizeof(struct cap_block), CAP_ALIGN);
    struct cap_block *blk;
    struct cap_block *prev = 0;

    if (ptr == 0) {
        return;
    }
    if ((uintptr_t)ptr < (CAP_HEAP_BASE_VA + hdr) || (uintptr_t)ptr >= cap_heap_cur) {
        cap_errno = CAP_EINVAL;
        return;
    }

    blk = (struct cap_block *)((uintptr_t)ptr - hdr);
    blk->free = 1;
    cap_coalesce_forward(blk);

    {
        struct cap_block *cur = cap_heap_head;
        while (cur != 0 && cur != blk) {
            prev = cur;
            cur = cur->next;
        }
    }
    if (prev != 0 && prev->free) {
        cap_coalesce_forward(prev);
    }
}
