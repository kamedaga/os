/* SPDX-License-Identifier: MIT */
/* Decision tests; real publication reads and native fault return have their
 * own memory-unit and QEMU ELF signal Gates. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/kobox2_adapter/exec_fault.c"

static const uint64_t site = 0x201000;
static unsigned char code[2] = {0xff, 0xd0};
static bool executable = true;
static unsigned captured;

bool ph_vm_read_executable(const struct ph_vm_space *space, uint64_t address,
    void *bytes, size_t length) {
    (void)space;
    assert(length == sizeof(code));
    if (!executable || address != site) {
        return false;
    }
    memcpy(bytes, code, length);
    return true;
}

void ph_vm_capture_syscall(struct ph_vm_space *space) {
    assert(space->syscall_in_fault);
    ++captured;
}

_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    fprintf(stderr, "%s:%u: %llu\n", file, line, (unsigned long long)result);
    abort();
}

static void rejected(struct ph_vm_space *space) {
    struct ph_vm_client_control before = *space->control;
    unsigned count = captured;

    assert(!ph_exec_capture_syscall_fault(space));
    assert(!space->syscall_in_fault && captured == count);
    assert(!memcmp(&before, space->control, sizeof(before)));
}

int main(void) {
    uint64_t sites[] = {site};
    struct ph_exec_image image = {.syscall_sites = sites, .site_count = 1, .elf_type = ET_EXEC};
    struct ph_vm_client_control valid = {
        .fault_ip = site, .fault_sp = 1, .fault_address = UINT64_MAX - 6,
        .fault_vector = 14, .fault_error = 6, .syscall_sequence = 7,
        .registers = {.rax = 15, .rip = site, .rsp = 1, .rflags = 0x202, .r12 = 0x1234},
    };
    struct ph_vm_client_control control;
    struct ph_vm_space space = {.control = &control, .exec_image = &image};

    for (unsigned offset = 0; offset < 8; ++offset) {
        control = valid;
        control.fault_address += offset;
        space.syscall_in_fault = false;
        assert(ph_exec_capture_syscall_fault(&space));
        assert(control.syscall_sequence == 8 && space.syscall_in_fault);
        assert(control.registers.rip == site + 2 && control.registers.rcx == site + 2);
        assert(control.registers.r11 == 0x202 && control.registers.rflags == 0x202);
        assert(control.registers.rax == 15 && control.registers.rsp == 1 &&
            control.registers.r12 == 0x1234);
    }
    space.syscall_in_fault = false;
    for (unsigned error = 0; error < 128; ++error) {
        if (error == 6 || error == 7) { continue; }
        control = valid;
        control.fault_error = error;
        rejected(&space);
    }
    for (unsigned bit = 7; bit < 64; ++bit) {
        control = valid;
        control.fault_error |= UINT64_C(1) << bit;
        rejected(&space);
    }
    control = valid;
    ++control.fault_ip;
    rejected(&space);
    control = valid;
    control.fault_address += 8;
    rejected(&space);
    control = valid;
    control.fault_vector = 13;
    rejected(&space);
    control = valid;
    executable = false;
    rejected(&space);
    executable = true;
    code[1] = 0x90;
    rejected(&space);
    code[1] = 0xd0;
    sites[0] = site + 2; /* Identical CALL bytes, but not a decoded SYSCALL site. */
    rejected(&space);
    sites[0] = site;
    image.elf_type = ET_DYN; /* No guessed load bias. */
    rejected(&space);
    image.elf_type = ET_EXEC;
    /* Removed INT3 entry must never be accepted as a syscall, even at a
     * recorded site with the former replacement bytes. */
    code[0] = 0xcc;
    code[1] = 0x90;
    control = valid;
    control.fault_vector = 3;
    control.fault_error = 0;
    control.fault_ip = site + 1;
    control.registers.rip = site + 1;
    rejected(&space);
    control = valid;
    /* The old opcode is also rejected when the event claims a page fault. */
    rejected(&space);
    code[0] = 0xff;
    code[1] = 0xd0;
    control = valid;
    control.fault_vector = 3;
    rejected(&space);
    control = valid;
    space.exec_image = NULL;
    rejected(&space);
    puts("KOBOX_EXEC_FAULT_UNIT=PASS provenance,opcode,mapping,fault,registers");
    return 0;
}
