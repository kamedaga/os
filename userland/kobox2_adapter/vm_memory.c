/* SPDX-License-Identifier: MIT */
/* Published host translations, not Linux VMAs or ownership of RAM pages. */
#include "vm_internal.h"
#include <errno.h>

struct ph_vm_mapping {
    uint64_t start, end, physical;
    unsigned protection;
};

struct ph_vm_memory {
    atomic_uint lock;
    struct ph_vm_space *members;
    struct ph_vm_mapping *mappings;
    size_t count, allocation;
};

void ph_vm_memory_create(struct ph_vm_space *space) {
    space->memory = ph_alloc(sizeof(*space->memory));
    space->memory->members = space;
}

uint64_t ph_vm_lock_memory(struct ph_vm_space *space) {
    uint64_t mask;

    PH_OK(ph_notifications_save(&mask));
    ph_lock(&space->memory->lock);
    return mask;
}

void ph_vm_unlock_memory(struct ph_vm_space *space, uint64_t mask) {
    ph_unlock(&space->memory->lock);
    PH_OK(ph_notifications_restore(mask));
}

void ph_vm_memory_destroy(struct ph_vm_space *space) {
    struct ph_vm_memory *memory = space->memory;
    uint64_t mask = ph_vm_lock_memory(space);
    struct ph_vm_space **member = &memory->members;

    while (*member && *member != space) {
        member = &(*member)->memory_next;
    }
    PH_CHECK(*member == space);
    *member = space->memory_next;
    bool last = memory->members == NULL;

    ph_vm_unlock_memory(space, mask);
    if (!last) {
        return;
    }

    if (memory->mappings) {
        ph_free(memory->mappings, memory->allocation);
    }
    ph_free(memory, sizeof(*memory));
}

static int native_map(struct ph_vm_space *space, uint64_t address, uint64_t physical,
    size_t length, unsigned protection) {
    long result = pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, space->process_fd,
        space->ram_grant_fds[protection], address, length, protection,
        physical | PACHA_PROCESS_MAP_SHARED | PACHA_PROCESS_MAP_REPLACE);

    return (uint64_t)result == address ? 0 : ph_vm_native_error(result);
}

static int native_reset(struct ph_vm_space *space, uint64_t address, size_t length) {
    return ph_vm_native_error(pacha_syscall4(PACHA_PROCESS_SYSCALL_UNMAP,
        space->process_fd, address, length, 0));
}

int ph_vm_memory_share(struct ph_vm_space *child, struct ph_vm_space *parent) {
    /* The new bootstrap is parked, unpublished and has no pending event.
     * Its producer cannot enter a memory operation until after attachment. */
    uint64_t mask = ph_vm_lock_memory(parent);
    struct ph_vm_memory *memory = parent->memory;
    int result = 0;

    for (size_t index = 0; index < memory->count; ++index) {
        const struct ph_vm_mapping *mapping = &memory->mappings[index];

        result = native_map(child, mapping->start, mapping->physical,
            mapping->end - mapping->start, mapping->protection);
        if (result) {
            break;
        }
    }
    if (!result) {
        ph_vm_memory_destroy(child);
        child->memory = memory;
        child->memory_next = memory->members;
        memory->members = child;
    }
    ph_vm_unlock_memory(parent, mask);
    return result;
}

static bool valid_range(const struct ph_vm_space *space, uint64_t address, size_t length) {
    return length && !((address | length) & (PH_PAGE_SIZE - 1)) &&
        address >= space->start && address - space->start <= space->length &&
        length <= space->length - (address - space->start);
}

/* The event producer holds the MM lock. Read only successfully published
 * immutable executable bytes through the parent's existing RAM alias. */
bool ph_vm_read_executable(const struct ph_vm_space *space, uint64_t address,
    void *bytes, size_t length) {
    const struct ph_vm_memory *memory = space->memory;
    size_t copied = 0;

    if (!bytes || !length || address > UINT64_MAX - length) {
        return false;
    }
    for (size_t index = 0; index < memory->count && copied < length; ++index) {
        const struct ph_vm_mapping *mapping = &memory->mappings[index];
        uint64_t cursor = address + copied;

        if (mapping->end <= cursor) {
            continue;
        }
        if (mapping->start > cursor || !(mapping->protection & KOBOX_VM_EXECUTE) ||
            (mapping->protection & KOBOX_VM_WRITE)) {
            return false;
        }
        size_t count = mapping->end - cursor;

        if (count > length - copied) {
            count = length - copied;
        }
        uint64_t physical = mapping->physical + cursor - mapping->start;

        PH_CHECK(physical <= PH_RAM_SIZE && count <= PH_RAM_SIZE - physical);
        memcpy((char *)bytes + copied, (char *)ph_core.direct + physical, count);
        copied += count;
    }
    return copied == length;
}

/* Construct the replacement before touching native mappings. At most two
 * old fragments and one replacement are introduced. Failure leaves both
 * the native tree and the authoritative publication inventory unchanged. */
static struct ph_vm_mapping *prepare_update(const struct ph_vm_memory *memory,
    uint64_t address, uint64_t physical, size_t length, unsigned protection,
    bool map, size_t *count, size_t *allocation) {
    if (memory->count > SIZE_MAX / sizeof(struct ph_vm_mapping) - 3) {
        return NULL;
    }
    *allocation = (memory->count + 3) * sizeof(struct ph_vm_mapping);
    if (*allocation > SIZE_MAX - PH_PAGE_SIZE + 1) {
        return NULL;
    }
    *allocation = (*allocation + PH_PAGE_SIZE - 1) & ~(PH_PAGE_SIZE - 1);
    long buffer = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, 0, 0, *allocation,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS, 0);

    if (buffer < (long)PH_PAGE_SIZE) {
        return NULL;
    }
    struct ph_vm_mapping *next = (void *)(uintptr_t)buffer;
    uint64_t end = address + length;
    bool inserted = !map;

    *count = 0;
    for (size_t index = 0; index < memory->count; ++index) {
        struct ph_vm_mapping old = memory->mappings[index];

        if (old.end <= address) {
            next[(*count)++] = old;
            continue;
        }
        if (old.start < address) {
            struct ph_vm_mapping left = old;

            left.end = address;
            next[(*count)++] = left;
        }
        if (!inserted) {
            next[(*count)++] = (struct ph_vm_mapping) {address, end, physical, protection};
            inserted = true;
        }
        if (old.end > end) {
            if (old.start < end) {
                old.physical += end - old.start;
                old.start = end;
            }
            next[(*count)++] = old;
        }
    }
    if (!inserted) {
        next[(*count)++] = (struct ph_vm_mapping) {address, end, physical, protection};
    }
    return next;
}

static void rollback_pages(struct ph_vm_space *space, uint64_t address, size_t length) {
    uint64_t mask = ph_vm_lock_space(space);

    if (!space->reaped) {
        PH_OK(native_reset(space, address, length));
        for (size_t index = 0; index < space->memory->count; ++index) {
            const struct ph_vm_mapping *mapping = &space->memory->mappings[index];
            uint64_t start = mapping->start > address ? mapping->start : address;
            uint64_t end = mapping->end < address + length ? mapping->end : address + length;

            if (start < end) {
                PH_OK(native_map(space, start, mapping->physical + start - mapping->start,
                    end - start, mapping->protection));
            }
        }
    }
    ph_vm_unlock_space(space, mask);
}

static int publish_members(struct ph_vm_memory *memory, uint64_t address, uint64_t physical,
    size_t length, unsigned protection, bool map) {
    bool alive = false;

    for (struct ph_vm_space *member = memory->members; member; member = member->memory_next) {
        uint64_t mask = ph_vm_lock_space(member);
        int result = 0;

        if (!member->reaped) {
            alive = true;
            result = map ? native_map(member, address, physical, length, protection) :
                native_reset(member, address, length);
        }
        ph_vm_unlock_space(member, mask);
        if (result) {
            /* The failing native operation is itself failure-atomic. Restore
             * earlier members from the uncommitted inventory before returning
             * an error. If restoration fails, continuing would be unsafe. */
            for (struct ph_vm_space *done = memory->members; done != member; done = done->memory_next) {
                rollback_pages(done, address, length);
            }
            return result;
        }
    }
    return map && !alive ? -ESRCH : 0;
}

static int update_pages(struct ph_vm_space *space, uint64_t address, uint64_t physical,
    size_t length, unsigned protection, bool map) {
    uint64_t memory_mask = ph_vm_lock_memory(space);
    struct ph_vm_memory *memory = space->memory;
    size_t count, allocation;
    int result = -ENOMEM;
    struct ph_vm_mapping *next = prepare_update(memory, address, physical, length,
        protection, map, &count, &allocation);

    if (next) {
        result = publish_members(memory, address, physical, length, protection, map);
        if (!result) {
            if (memory->mappings) {
                ph_free(memory->mappings, memory->allocation);
            }
            memory->mappings = next;
            memory->count = count;
            memory->allocation = allocation;
        } else {
            ph_free(next, allocation);
        }
    }
    ph_vm_unlock_memory(space, memory_mask);
    return result;
}

int ph_vm_map_pages(void *opaque, uint64_t address, uint64_t physical,
    size_t length, unsigned protection) {
    struct ph_vm_space *space = opaque;

    if (!valid_range(space, address, length) || (physical & (PH_PAGE_SIZE - 1)) ||
        physical > PH_RAM_SIZE || length > PH_RAM_SIZE - physical ||
        (protection & ~(KOBOX_VM_READ | KOBOX_VM_WRITE | KOBOX_VM_EXECUTE))) {
        return -EINVAL;
    }
    return update_pages(space, address, physical, length, protection, true);
}

int ph_vm_reset_pages(void *opaque, uint64_t address, size_t length) {
    struct ph_vm_space *space = opaque;

    if (!valid_range(space, address, length)) {
        return -EINVAL;
    }
    return update_pages(space, address, 0, length, 0, false);
}

uint64_t ph_vm_guest_fault_error(const struct ph_vm_space *space) {
    const struct ph_vm_client_control *control = space->control;
    uint64_t error = control->fault_error;

    /* PachaOS retains supervisor identity entries under its user CR3.
     * A hole in our publication inventory therefore need not produce a
     * hardware not-present fault. Translate only ordinary user protection
     * faults within the owned Linux window; retain raw error in control.
     * RSVD/PK/SGX and faults in native bootstrap/page0 are never normalized. */
    if ((error & 5) != 5 || (error & ~UINT64_C(0x17)) ||
        control->fault_address < space->start ||
        control->fault_address - space->start >= space->length) {
        return error;
    }
    unsigned access = (error & 2) ? KOBOX_VM_WRITE :
        (error & 16) ? KOBOX_VM_EXECUTE : KOBOX_VM_READ;
    const struct ph_vm_memory *memory = space->memory;

    for (size_t index = 0; index < memory->count; ++index) {
        const struct ph_vm_mapping *mapping = &memory->mappings[index];

        if (control->fault_address < mapping->start) {
            break;
        }
        if (control->fault_address < mapping->end) {
            /* A publication may have overtaken delivery of an earlier
             * fault. Retry through Linux only if it now permits access;
             * published RO/NX/PROT_NONE violations remain violations. */
            return mapping->protection & access ? error & ~UINT64_C(1) : error;
        }
    }
    return error & ~UINT64_C(1);
}
