/* SPDX-License-Identifier: MIT */
/* Inventory/transaction unit tests. Native PTE behavior is tested in QEMU. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/kobox2_adapter/vm_memory.c"

#define TEST_PAGES 32
#define TEST_BASE UINT64_C(0x10000)
struct test_page { uint64_t physical; unsigned protection; bool mapped; };
static struct test_page native_pages[3][TEST_PAGES];
static int fail_process;
static bool fail_allocation;
static size_t allocations;
static unsigned native_calls;
struct ph_image ph_core;

void *ph_alloc(size_t size) {
    void *result = calloc(1, size);
    assert(result);
    ++allocations;
    return result;
}
void ph_free(void *base, size_t size) {
    (void)size;
    assert(base && allocations);
    --allocations;
    free(base);
}
_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    fprintf(stderr, "%s:%u: %llu\n", file, line, (unsigned long long)result);
    abort();
}
int ph_notifications_save(uint64_t *mask) { *mask = 0; return 0; }
int ph_notifications_restore(uint64_t mask) { assert(!mask); return 0; }
void ph_lock(atomic_uint *lock) { assert(!atomic_exchange(lock, 1)); }
void ph_unlock(atomic_uint *lock) { assert(atomic_exchange(lock, 0) == 1); }
uint64_t ph_vm_lock_space(struct ph_vm_space *space) { ph_lock(&space->lock); return 0; }
void ph_vm_unlock_space(struct ph_vm_space *space, uint64_t mask) {
    assert(!mask);
    ph_unlock(&space->lock);
}
int ph_vm_native_error(long result) { return result ? -EFAULT : 0; }

long pacha_syscall6(uint64_t number, uint64_t process, uint64_t fd, uint64_t address,
    uint64_t length, uint64_t protection, uint64_t flags) {
    ++native_calls;
    if (number == PACHA_VM_SYSCALL_MMAP) {
        assert(address && !(address % PH_PAGE_SIZE)); /* third argument is size */
        if (fail_allocation) {
            fail_allocation = false;
            return PACHA_SYSCALL_ERR_ALLOC;
        }
        return (long)(uintptr_t)ph_alloc(address);
    }
    assert(number == PACHA_PROCESS_SYSCALL_MAP && process >= 1 && process <= 3);
    assert(fd == protection + 16);
    if (fail_process == (int)process) {
        fail_process = 0;
        return PACHA_SYSCALL_ERR_MAP;
    }
    size_t first = (address - TEST_BASE) / PH_PAGE_SIZE;
    size_t count = length / PH_PAGE_SIZE;
    uint64_t physical = flags & ~(PH_PAGE_SIZE - 1);

    assert(first + count <= TEST_PAGES);
    for (size_t index = 0; index < count; ++index) {
        native_pages[process - 1][first + index] = (struct test_page) {
            physical + index * PH_PAGE_SIZE, protection, true,
        };
    }
    return address;
}
long pacha_syscall4(uint64_t number, uint64_t process, uint64_t address,
    uint64_t length, uint64_t flags) {
    ++native_calls;
    assert(number == PACHA_PROCESS_SYSCALL_UNMAP && !flags && process >= 1 && process <= 3);
    if (fail_process == (int)process) {
        fail_process = 0;
        return PACHA_SYSCALL_ERR_MAP;
    }
    size_t first = (address - TEST_BASE) / PH_PAGE_SIZE;
    size_t count = length / PH_PAGE_SIZE;

    assert(first + count <= TEST_PAGES);
    memset(&native_pages[process - 1][first], 0, count * sizeof(struct test_page));
    return 0;
}

static void check_inventory(const struct ph_vm_space *space) {
    const struct ph_vm_memory *memory = space->memory;
    struct test_page expected[TEST_PAGES] = {0};
    uint64_t previous = TEST_BASE;

    for (size_t index = 0; index < memory->count; ++index) {
        const struct ph_vm_mapping *mapping = &memory->mappings[index];

        assert(mapping->start >= previous && mapping->start < mapping->end);
        previous = mapping->end;
        for (uint64_t address = mapping->start; address < mapping->end; address += PH_PAGE_SIZE) {
            size_t page = (address - TEST_BASE) / PH_PAGE_SIZE;

            assert(page < TEST_PAGES);
            expected[page] = (struct test_page) {
                mapping->physical + address - mapping->start, mapping->protection, true,
            };
        }
    }
    for (const struct ph_vm_space *member = memory->members; member; member = member->memory_next) {
        if (member->reaped) {
            continue;
        }
        for (unsigned page = 0; page < TEST_PAGES; ++page) {
            const struct test_page *actual = &native_pages[member->process_fd - 1][page];

            assert(actual->mapped == expected[page].mapped);
            if (actual->mapped) {
                assert(actual->physical == expected[page].physical);
                assert(actual->protection == expected[page].protection);
            }
        }
    }
}

static void check_invalid_requests(struct ph_vm_space *space) {
    const struct {
        uint64_t address;
        size_t length;
    } ranges[] = {
        {TEST_BASE, 0},
        {TEST_BASE + 1, PH_PAGE_SIZE},
        {TEST_BASE, PH_PAGE_SIZE - 1},
        {TEST_BASE - PH_PAGE_SIZE, PH_PAGE_SIZE},
        {TEST_BASE + space->length, PH_PAGE_SIZE},
        {TEST_BASE + space->length - PH_PAGE_SIZE, 2 * PH_PAGE_SIZE},
        {UINT64_MAX & ~(PH_PAGE_SIZE - 1), 2 * PH_PAGE_SIZE},
        {TEST_BASE, SIZE_MAX & ~(PH_PAGE_SIZE - 1)},
    };
    const uint64_t physical[] = {1, PH_RAM_SIZE, UINT64_MAX & ~(PH_PAGE_SIZE - 1)};
    unsigned calls_before = native_calls;
    size_t allocations_before = allocations;
    const struct ph_vm_mapping *mappings_before = space->memory->mappings;
    size_t count_before = space->memory->count;

    for (unsigned index = 0; index < sizeof(ranges) / sizeof(ranges[0]); ++index) {
        assert(ph_vm_map_pages(space, ranges[index].address, 0,
            ranges[index].length, KOBOX_VM_READ) == -EINVAL);
        assert(ph_vm_reset_pages(space, ranges[index].address,
            ranges[index].length) == -EINVAL);
    }
    for (unsigned index = 0; index < sizeof(physical) / sizeof(physical[0]); ++index) {
        assert(ph_vm_map_pages(space, TEST_BASE, physical[index],
            PH_PAGE_SIZE, KOBOX_VM_READ) == -EINVAL);
    }
    assert(ph_vm_map_pages(space, TEST_BASE, PH_RAM_SIZE - PH_PAGE_SIZE,
        2 * PH_PAGE_SIZE, KOBOX_VM_READ) == -EINVAL);
    assert(ph_vm_map_pages(space, TEST_BASE, 0, PH_PAGE_SIZE, 8) == -EINVAL);
    /* Validation must precede native mapping and even transaction allocation. */
    assert(native_calls == calls_before && allocations == allocations_before);
    assert(space->memory->mappings == mappings_before && space->memory->count == count_before);
    check_inventory(space);
}

static void check_faults(struct ph_vm_space *space) {
    struct ph_vm_client_control control = {0};

    space->control = &control;
    assert(!ph_vm_reset_pages(space, TEST_BASE, TEST_PAGES * PH_PAGE_SIZE));
    control.fault_address = TEST_BASE;
    control.fault_error = 0x15;
    assert(ph_vm_guest_fault_error(space) == 0x14);
    for (unsigned protection = 0; protection < 8; ++protection) {
        assert(!ph_vm_map_pages(space, TEST_BASE, 0, PH_PAGE_SIZE, protection));
        const unsigned errors[] = {5, 7, 21};
        const unsigned access[] = {KOBOX_VM_READ, KOBOX_VM_WRITE, KOBOX_VM_EXECUTE};

        for (unsigned index = 0; index < 3; ++index) {
            control.fault_error = errors[index];
            assert(ph_vm_guest_fault_error(space) ==
                (protection & access[index] ? errors[index] & ~1U : errors[index]));
        }
    }
    for (unsigned bit = 3; bit < 64; ++bit) {
        if (bit == 4) { continue; }
        control.fault_error = 5 | (UINT64_C(1) << bit);
        assert(ph_vm_guest_fault_error(space) == control.fault_error);
    }
    control.fault_error = 5;
    control.fault_address = 0;
    assert(ph_vm_guest_fault_error(space) == 5);
    control.fault_address = space->start + space->length;
    assert(ph_vm_guest_fault_error(space) == 5);
    space->control = NULL;
}

static void check_executable_reads(struct ph_vm_space *space) {
    unsigned char ram[3 * PH_PAGE_SIZE] = {0};
    unsigned char bytes[2];
    uint64_t address = TEST_BASE + PH_PAGE_SIZE - 1;

    ph_core.direct = ram;
    assert(!ph_vm_reset_pages(space, TEST_BASE, TEST_PAGES * PH_PAGE_SIZE));
    assert(!ph_vm_read_executable(space, address, bytes, 2));
    assert(!ph_vm_map_pages(space, TEST_BASE, 0, PH_PAGE_SIZE, KOBOX_VM_READ | KOBOX_VM_EXECUTE));
    assert(!ph_vm_map_pages(space, TEST_BASE + PH_PAGE_SIZE, 2 * PH_PAGE_SIZE,
        PH_PAGE_SIZE, KOBOX_VM_READ | KOBOX_VM_EXECUTE));
    ram[PH_PAGE_SIZE - 1] = 0xff;
    ram[2 * PH_PAGE_SIZE] = 0xd0;
    assert(ph_vm_read_executable(space, address, bytes, 2));
    assert(bytes[0] == 0xff && bytes[1] == 0xd0);
    assert(!ph_vm_read_executable(space, UINT64_MAX, bytes, 2));
    assert(!ph_vm_read_executable(space, address, NULL, 2));
    assert(!ph_vm_read_executable(space, address, bytes, 0));
    for (unsigned protection = 0; protection < 8; ++protection) {
        assert(!ph_vm_map_pages(space, TEST_BASE + PH_PAGE_SIZE,
            2 * PH_PAGE_SIZE, PH_PAGE_SIZE, protection));
        assert(ph_vm_read_executable(space, address, bytes, 2) ==
            (!!(protection & KOBOX_VM_EXECUTE) && !(protection & KOBOX_VM_WRITE)));
    }
    assert(!ph_vm_reset_pages(space, TEST_BASE + PH_PAGE_SIZE, PH_PAGE_SIZE));
    assert(!ph_vm_read_executable(space, address, bytes, 2));
    ph_core.direct = NULL;
}

int main(void) {
    struct ph_vm_space spaces[3] = {0};

    for (unsigned index = 0; index < 3; ++index) {
        spaces[index].process_fd = index + 1;
        spaces[index].start = TEST_BASE;
        spaces[index].length = TEST_PAGES * PH_PAGE_SIZE;
        for (unsigned prot = 0; prot < 8; ++prot) {
            spaces[index].ram_grant_fds[prot] = prot + 16;
        }
        ph_vm_memory_create(&spaces[index]);
    }
    assert(!ph_vm_map_pages(&spaces[0], TEST_BASE, 0, TEST_PAGES * PH_PAGE_SIZE, 7));
    assert(!ph_vm_memory_share(&spaces[1], &spaces[0]));
    assert(!ph_vm_memory_share(&spaces[2], &spaces[0]));
    check_inventory(&spaces[0]);
    check_invalid_requests(&spaces[0]);
    uint32_t random = 0x71283891;

    for (unsigned trial = 0; trial < 10000; ++trial) {
        random = random * 1664525U + 1013904223U;
        unsigned first = (random >> 16) % TEST_PAGES;
        unsigned count = 1 + (random >> 8) % (TEST_PAGES - first);
        uint64_t address = TEST_BASE + first * PH_PAGE_SIZE;
        bool map = random & 1;
        struct test_page before[3][TEST_PAGES];

        memcpy(before, native_pages, sizeof(before));
        fail_process = trial % 5 == 0 ? 1 + trial % 3 : 0;
        fail_allocation = trial % 7 == 0;
        bool failure = fail_process || fail_allocation;
        int result = map ? ph_vm_map_pages(&spaces[trial % 3], address,
            (random % 512) * PH_PAGE_SIZE, count * PH_PAGE_SIZE, (random >> 2) & 7) :
            ph_vm_reset_pages(&spaces[trial % 3], address, count * PH_PAGE_SIZE);

        assert(failure == (result != 0));
        if (failure) {
            /* Compare fields: C struct padding is not part of the model. */
            for (unsigned proc = 0; proc < 3; ++proc) {
                for (unsigned page = 0; page < TEST_PAGES; ++page) {
                    assert(native_pages[proc][page].mapped == before[proc][page].mapped);
                    assert(native_pages[proc][page].physical == before[proc][page].physical);
                    assert(native_pages[proc][page].protection == before[proc][page].protection);
                }
            }
        }
        fail_process = 0;
        fail_allocation = false;
        check_inventory(&spaces[0]);
    }
    check_faults(&spaces[0]);
    check_executable_reads(&spaces[0]);
    spaces[0].reaped = true;
    assert(!ph_vm_map_pages(&spaces[0], TEST_BASE, 0, PH_PAGE_SIZE, 3));
    check_inventory(&spaces[1]);
    ph_vm_memory_destroy(&spaces[0]);
    check_inventory(&spaces[1]);
    ph_vm_memory_destroy(&spaces[1]);
    ph_vm_memory_destroy(&spaces[2]);
    assert(!allocations);
    puts("KOBOX_VM_MEMORY_UNIT=PASS trials=10000 shared=3 rollback=map,reset,allocation invalid=21");
    return 0;
}
